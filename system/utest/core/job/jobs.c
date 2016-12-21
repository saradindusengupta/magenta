// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <magenta/syscalls.h>
#include <mini-process/mini-process.h>
#include <unittest/unittest.h>

static const char process_name[] = "job-test-p";

extern mx_handle_t root_job;

static bool basic_test(void) {
    BEGIN_TEST;

    // Never close the launchpad job.
    mx_handle_t job_parent = root_job;
    ASSERT_NEQ(job_parent, MX_HANDLE_INVALID, "");

    // If the parent job is valid, one should be able to create a child job
    // and a child job of the child job.
    mx_handle_t job_child, job_grandchild;
    ASSERT_EQ(mx_job_create(job_parent, 0u, &job_child), NO_ERROR, "");
    ASSERT_EQ(mx_job_create(job_child, 0u, &job_grandchild), NO_ERROR, "");

    ASSERT_EQ(mx_handle_close(job_child), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(job_grandchild), NO_ERROR, "");

    // If the parent job is not valid it should fail.
    mx_handle_t job_fail;
    ASSERT_EQ(mx_job_create(MX_HANDLE_INVALID, 0u, &job_fail), ERR_BAD_HANDLE, "");

    END_TEST;
}

static bool create_test(void) {
    BEGIN_TEST;

    mx_handle_t job_parent = root_job;
    ASSERT_NEQ(job_parent, MX_HANDLE_INVALID, "");

    mx_handle_t job_child;
    ASSERT_EQ(mx_job_create(job_parent, 0u, &job_child), NO_ERROR, "");

    // Make sure we can create process object with both the parent job and a child job.
    mx_handle_t process1, vmar1;
    ASSERT_EQ(mx_process_create(
        job_parent, process_name, sizeof(process_name), 0u, &process1, &vmar1), NO_ERROR, "");

    mx_handle_t process2, vmar2;
    ASSERT_EQ(mx_process_create(
        job_child, process_name, sizeof(process_name), 0u, &process2, &vmar2), NO_ERROR, "");

    ASSERT_EQ(mx_handle_close(job_child), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(process1), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(process2), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(vmar1), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(vmar2), NO_ERROR, "");

    END_TEST;
}

static bool kill_test(void) {
    BEGIN_TEST;

    mx_handle_t job_parent = root_job;
    ASSERT_NEQ(job_parent, MX_HANDLE_INVALID, "");

    mx_handle_t job_child;
    ASSERT_EQ(mx_job_create(job_parent, 0u, &job_child), NO_ERROR, "");

    mx_handle_t process, thread;
    ASSERT_EQ(start_mini_process(job_child, &process, &thread), NO_ERROR, "");

    mx_nanosleep(5000000);
    ASSERT_EQ(mx_task_kill(job_child), NO_ERROR, "");

    mx_signals_t signals;
    mx_handle_wait_one(process, MX_PROCESS_SIGNALED, 0u, &signals);

    END_TEST;
}

BEGIN_TEST_CASE(job_tests)
RUN_TEST(basic_test)
RUN_TEST(create_test)
RUN_TEST(kill_test)
END_TEST_CASE(job_tests)