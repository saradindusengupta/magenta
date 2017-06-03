// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "iommu_impl.h"

// TODO(teisenbe): Remove this arch/x86 dep once we start dynamically allocating
// the fault IRQ
#include <arch/x86/interrupts.h>

#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_aspace.h>
#include <kernel/vm/vm_object_paged.h>
#include <mxalloc/new.h>
#include <mxtl/algorithm.h>
#include <mxtl/ref_ptr.h>
#include <platform.h>
#include <trace.h>

#include "context_table_state.h"
#include "device_context.h"
#include "hw.h"

#define LOCAL_TRACE 1

namespace intel_iommu {

IommuImpl::IommuImpl(uint64_t id, volatile void* register_base)
    : Iommu(id), mmio_(register_base) {
}

mxtl::RefPtr<Iommu> IommuImpl::Create(uint64_t id, paddr_t register_base) {
    auto kernel_aspace = VmAspace::kernel_aspace();
    void *vaddr;
    status_t status = kernel_aspace->AllocPhysical(
            "iommu",
            PAGE_SIZE,
            &vaddr,
            PAGE_SIZE_SHIFT,
            register_base,
            0,
            ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE);
    if (status != NO_ERROR) {
        return nullptr;
    }

    AllocChecker ac;
    auto instance = mxtl::AdoptRef<IommuImpl>(new (&ac) IommuImpl(id, vaddr));
    if (!ac.check()) {
        kernel_aspace->FreeRegion(reinterpret_cast<vaddr_t>(vaddr));
        return nullptr;
    }

    status = instance->Initialize();
    if (status != NO_ERROR) {
        return nullptr;
    }

    RegisterIommu(instance);
    return instance;
}

IommuImpl::~IommuImpl() {
    AutoLock guard(&lock_);

    // We cannot unpin memory until translation is disabled
    status_t status = SetTranslationEnableLocked(false, INFINITE_TIME);
    ASSERT(status == NO_ERROR);

    DisableFaultsLocked();

    VmAspace::kernel_aspace()->FreeRegion(mmio_.base());
}

bool IommuImpl::IsValidBusTxnId(uint64_t bus_txn_id) const {
    // TODO(teisenbe): Decode the txn id and check against configuration.
    if (bus_txn_id > UINT16_MAX) {
        return false;
    }
    return true;
}

status_t IommuImpl::Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                             dev_vaddr_t* vaddr) {
    DEBUG_ASSERT(vaddr);
    if (!IS_PAGE_ALIGNED(paddr) || !IS_PAGE_ALIGNED(size)) {
        return ERR_INVALID_ARGS;
    }
    if (perms & ~(IOMMU_FLAG_PERM_READ | IOMMU_FLAG_PERM_WRITE | IOMMU_FLAG_PERM_EXECUTE)) {
        return ERR_INVALID_ARGS;
    }
    if (perms == 0) {
        return ERR_INVALID_ARGS;
    }
    return NO_ERROR;
}

status_t IommuImpl::Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) {
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size)) {
        return ERR_INVALID_ARGS;
    }
    return NO_ERROR;
}

status_t IommuImpl::ClearMappingsForBusTxnId(uint64_t bus_txn_id) {
    return NO_ERROR;
}

status_t IommuImpl::Initialize() {
    AutoLock guard(&lock_);

    // Ensure we support this device version
    auto version = reg::Version::Get().ReadFrom(&mmio_);
    if (version.major() != 1 && version.minor() != 0) {
        LTRACEF("Unsupported IOMMU version: %u.%u\n", version.major(), version.minor());
        return ERR_NOT_SUPPORTED;
    }

    // Cache useful capability info
    caps_ = reg::Capability::Get().ReadFrom(&mmio_);
    extended_caps_ = reg::ExtendedCapability::Get().ReadFrom(&mmio_);

    max_guest_addr_mask_ = (1ULL << (caps_.max_guest_addr_width() + 1)) - 1;
    fault_recording_reg_offset_ = static_cast<uint32_t>(
            caps_.fault_recording_register_offset() * 16);
    num_fault_recording_reg_ = static_cast<uint32_t>(caps_.num_fault_recording_reg() + 1);
    iotlb_reg_offset_ = static_cast<uint32_t>(extended_caps_.iotlb_register_offset() * 16);
    if (iotlb_reg_offset_ > PAGE_SIZE - 16) {
        LTRACEF("Unsupported IOMMU: IOTLB offset runs past the register page\n");
        return ERR_NOT_SUPPORTED;
    }
    supports_extended_context_ = extended_caps_.supports_extended_context();
    if (extended_caps_.supports_pasid()) {
        valid_pasid_mask_ = static_cast<uint32_t>((1ULL << (extended_caps_.pasid_size() + 1)) - 1);
    }

    const uint64_t num_domains = caps_.num_domains();
    if (num_domains > 0x6) {
        LTRACEF("Unknown num_domains value\n");
        return ERR_NOT_SUPPORTED;
    }
    num_supported_domains_ = static_cast<uint32_t>(4 + 2 * num_domains);

    // Sanity check initial configuration
    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    if (global_ctl.translation_enable()) {
        LTRACEF("DMA remapping already enabled?!\n");
        return ERR_BAD_STATE;
    }
    if (global_ctl.interrupt_remap_enable()) {
        LTRACEF("IRQ remapping already enabled?!\n");
        return ERR_BAD_STATE;
    }

    // Allocate and setup the root table
    status_t status = IommuPage::AllocatePage(&root_table_page_);
    if (status != NO_ERROR) {
        LTRACEF("alloc root table failed\n");
        return status;
    }
    status = SetRootTablePointerLocked(root_table_page_.paddr());
    if (status != NO_ERROR) {
        LTRACEF("set root table failed\n");
        return status;
    }

    // Enable interrupts before we enable translation
    status = IommuImpl::ConfigureFaultEventInterruptLocked();
    if (status != NO_ERROR) {
        LTRACEF("configuring fault event irq failed\n");
        return status;
    }

    status = SetTranslationEnableLocked(true, current_time() + LK_SEC(1));
    if (status != NO_ERROR) {
        LTRACEF("set translation enable failed\n");
        return status;
    }

    DeviceContext* context;
    status = InitializeContextLocked(0, 0x2 << 3, &context);
    if (status != NO_ERROR) {
        LTRACEF("initialize gfx context failed\n");
        return status;
    }

    return NO_ERROR;
}

// Sets the root table pointer and invalidates the context-cache and IOTLB.
status_t IommuImpl::SetRootTablePointerLocked(paddr_t pa) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(pa));

    auto root_table_addr = reg::RootTableAddress::Get().FromValue(0);
    // If we support extended contexts, use it.
    root_table_addr.set_root_table_type(supports_extended_context_);
    root_table_addr.set_root_table_address(pa >> PAGE_SIZE_SHIFT);
    root_table_addr.WriteTo(&mmio_);

    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    DEBUG_ASSERT(!global_ctl.translation_enable());
    global_ctl.set_root_table_ptr(1);
    global_ctl.WriteTo(&mmio_);
    status_t status = WaitForValueLocked(&global_ctl, &decltype(global_ctl)::root_table_ptr,
                                         1, current_time() + LK_SEC(1));
    if (status != NO_ERROR) {
        LTRACEF("Timed out waiting for root_table_ptr bit to take\n");
        return status;
    }

    status = InvalidateContextCacheGlobalLocked();
    if (status != NO_ERROR) {
        return status;
    }

    status = InvalidateIotlbGlobalLocked();
    if (status != NO_ERROR) {
        return status;
    }

    return NO_ERROR;
}

status_t IommuImpl::SetTranslationEnableLocked(bool enabled, lk_time_t deadline) {
    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    global_ctl.set_translation_enable(enabled);
    global_ctl.WriteTo(&mmio_);

    return WaitForValueLocked(&global_ctl, &decltype(global_ctl)::translation_enable,
                              enabled, deadline);
}

status_t IommuImpl::InvalidateContextCacheGlobalLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto context_cmd = reg::ContextCommand::Get().FromValue(0);
    context_cmd.set_invld_context_cache(1);
    // TODO: make this an enum
    context_cmd.set_invld_request_granularity(1);
    context_cmd.WriteTo(&mmio_);

    return WaitForValueLocked(&context_cmd, &decltype(context_cmd)::invld_context_cache, 0,
                              INFINITE_TIME);
}

status_t IommuImpl::InvalidateIotlbGlobalLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto iotlb_invld = reg::IotlbInvalidate::Get(iotlb_reg_offset_).ReadFrom(&mmio_);
    iotlb_invld.set_invld_iotlb(1);
    // TODO: make this an enum
    iotlb_invld.set_invld_request_granularity(1);
    iotlb_invld.WriteTo(&mmio_);

    return WaitForValueLocked(&iotlb_invld, &decltype(iotlb_invld)::invld_iotlb, 0,
                              INFINITE_TIME);
}

template <class RegType>
status_t IommuImpl::WaitForValueLocked(RegType* reg,
                                        typename RegType::ValueType (RegType::*getter)(),
                                        typename RegType::ValueType value,
                                        lk_time_t deadline) {
    DEBUG_ASSERT(lock_.IsHeld());

    const lk_time_t kMaxSleepDuration = LK_USEC(10);

    while (true) {
        reg->ReadFrom(&mmio_);
        if ((reg->*getter)() == value) {
            return NO_ERROR;
        }

        const lk_time_t now = current_time();
        if (now > deadline) {
            break;
        }

        lk_time_t sleep_deadline = mxtl::min(now + kMaxSleepDuration, deadline);
        thread_sleep(sleep_deadline);
    }
    return ERR_TIMED_OUT;
}

// TODO: Remove this
static hwreg::RegisterIo* iommu_regs;
extern "C" void iommu_fault_handler();
void iommu_fault_handler() {
    TRACEF("Received IOMMU fault\n");
    auto status = reg::FaultStatus::Get().ReadFrom(iommu_regs);

    if (!status.primary_pending_fault()) {
        TRACEF("Non primary fault\n");
        return;
    }

    auto caps = reg::Capability::Get().ReadFrom(iommu_regs);
    const uint32_t num_regs = static_cast<uint32_t>(caps.num_fault_recording_reg() + 1);
    const uint32_t reg_offset = static_cast<uint32_t>(caps.fault_recording_register_offset() * 16);

    uint32_t index = status.fault_record_index();
    while (1) {
        auto rec_high = reg::FaultRecordHigh::Get(reg_offset, index).ReadFrom(iommu_regs);
        if (!rec_high.fault()) {
            break;
        }
        auto rec_low = reg::FaultRecordLow::Get(reg_offset, index).ReadFrom(iommu_regs);
        uint64_t source = rec_high.source_id();
        TRACEF("IOMMU Fault: access %c, PASID (%c) %#04lx, reason %#02lx, source %02lx:%02lx.%lx, info: %lx\n",
               rec_high.request_type() ? 'R' : 'W',
               rec_high.pasid_present() ? 'V' : '-',
               rec_high.pasid_value(),
               rec_high.fault_reason(),
               source >> 8, (source >> 3) & 0x1f, source & 0x7,
               rec_low.fault_info() << 12);

        // Clear this fault (RW1CS)
        rec_high.WriteTo(iommu_regs);

        ++index;
        if (index >= num_regs) {
            index -= num_regs;
        }
    }

    status.set_reg_value(0);
    // Clear the primary fault overflow condition (RW1CS)
    // TODO(teisenbe): How do we guarantee we get an interrupt on the next fault/if we left a fault unprocessed?
    status.set_primary_fault_overflow(1);
    status.WriteTo(iommu_regs);
}

status_t IommuImpl::ConfigureFaultEventInterruptLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto event_data = reg::FaultEventData::Get().FromValue(0);
    auto event_addr = reg::FaultEventAddress::Get().FromValue(0);
    auto event_upper_addr = reg::FaultEventUpperAddress::Get().FromValue(0);

    event_data.set_interrupt_message_data(X86_INT_IOMMU_FAULT);
    // TODO(teisenbe): Change this behavior
    // Send all interrupts to APIC 0
    event_addr.set_message_address(0xfee00000 >> 2);

    event_data.WriteTo(&mmio_);
    event_addr.WriteTo(&mmio_);
    event_upper_addr.WriteTo(&mmio_);

    // Clear all primary fault records
    for (uint32_t i = 0; i < num_fault_recording_reg_; ++i) {
        const uint32_t offset = fault_recording_reg_offset_;
        auto record_high = reg::FaultRecordHigh::Get(offset, i).ReadFrom(&mmio_);
        record_high.WriteTo(&mmio_);
    }

    // Clear all pending faults
    auto fault_status_ctl = reg::FaultStatus::Get().ReadFrom(&mmio_);
    fault_status_ctl.WriteTo(&mmio_);

    // TODO: remove
    iommu_regs = &mmio_;

    // Unmask interrupts
    auto fault_event_ctl = reg::FaultEventControl::Get().ReadFrom(&mmio_);
    fault_event_ctl.set_interrupt_mask(0);
    fault_event_ctl.WriteTo(&mmio_);

    return NO_ERROR;
}

void IommuImpl::DisableFaultsLocked() {
    auto fault_event_ctl = reg::FaultEventControl::Get().ReadFrom(&mmio_);
    fault_event_ctl.set_interrupt_mask(1);
    fault_event_ctl.WriteTo(&mmio_);
}

status_t IommuImpl::GetOrCreateContextTableLocked(uint8_t bus, uint8_t dev_func,
                                                  ContextTableState** tbl) {
    DEBUG_ASSERT(lock_.IsHeld());

    volatile ds::RootTable* root_table = this->root_table();
    DEBUG_ASSERT(root_table);

    volatile ds::RootEntrySubentry* target_entry = &root_table->entry[bus].lower;
    if (supports_extended_context_ && dev_func >= 0x80) {
        // If this is an extended root table and the device is in the upper half
        // of the bus address space, use the upper pointer.
        target_entry = &root_table->entry[bus].upper;
    }

    ds::RootEntrySubentry entry;
    entry.ReadFrom(target_entry);
    if (entry.present()) {
        // We know the entry exists, so search our list of tables for it.
        for (ContextTableState& context_table : context_tables_) {
            if (context_table.includes_bdf(bus, dev_func)) {
                *tbl = &context_table;
                return NO_ERROR;
            }
        }
    }

    // Couldn't find the ContextTable, so create it.
    mxtl::unique_ptr<ContextTableState> table;
    status_t status = ContextTableState::Create(bus, supports_extended_context_,
                                                dev_func >= 0x80 /* upper */,
                                                this,
                                                target_entry,
                                                &table);
    if (status != NO_ERROR) {
        return status;
    }

    *tbl = table.get();
    context_tables_.push_back(mxtl::move(table));

    return NO_ERROR;
}

status_t IommuImpl::InitializeContextLocked(uint8_t bus, uint8_t dev_func,
                                            DeviceContext** context) {
    DEBUG_ASSERT(lock_.IsHeld());

    ContextTableState* ctx_table_state;
    status_t status = GetOrCreateContextTableLocked(bus, dev_func, &ctx_table_state);
    if (status != NO_ERROR) {
        return status;
    }

    return ctx_table_state->CreateDeviceContext(bus, dev_func, context);
}

} // namespace intel_iommu
