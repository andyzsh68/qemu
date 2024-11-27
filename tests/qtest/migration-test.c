/*
 * QTest testcase for migration
 *
 * Copyright (c) 2016-2018 Red Hat, Inc. and/or its affiliates
 *   based on the vhost-user-test.c that is:
 *      Copyright (c) 2014 Virtual Open Systems Sarl.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qapi/qmp/qlist.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/range.h"
#include "qemu/sockets.h"
#include "chardev/char.h"
#include "crypto/tlscredspsk.h"
#include "ppc-util.h"

#include "migration/bootfile.h"
#include "migration/framework.h"
#include "migration/migration-qmp.h"
#include "migration/migration-util.h"

#define ANALYZE_SCRIPT "scripts/analyze-migration.py"

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif

static char *tmpfs;

static void test_baddest(void)
{
    MigrateStart args = {
        .hide_stderr = true
    };
    QTestState *from, *to;

    if (migrate_start(&from, &to, "tcp:127.0.0.1:0", &args)) {
        return;
    }
    migrate_qmp(from, to, "tcp:127.0.0.1:0", NULL, "{}");
    wait_for_migration_fail(from, false);
    migrate_end(from, to, false);
}

#ifndef _WIN32
static void test_analyze_script(void)
{
    MigrateStart args = {
        .opts_source = "-uuid 11111111-1111-1111-1111-111111111111",
    };
    QTestState *from, *to;
    g_autofree char *uri = NULL;
    g_autofree char *file = NULL;
    int pid, wstatus;
    const char *python = g_getenv("PYTHON");

    if (!python) {
        g_test_skip("PYTHON variable not set");
        return;
    }

    /* dummy url */
    if (migrate_start(&from, &to, "tcp:127.0.0.1:0", &args)) {
        return;
    }

    /*
     * Setting these two capabilities causes the "configuration"
     * vmstate to include subsections for them. The script needs to
     * parse those subsections properly.
     */
    migrate_set_capability(from, "validate-uuid", true);
    migrate_set_capability(from, "x-ignore-shared", true);

    file = g_strdup_printf("%s/migfile", tmpfs);
    uri = g_strdup_printf("exec:cat > %s", file);

    migrate_ensure_converge(from);
    migrate_qmp(from, to, uri, NULL, "{}");
    wait_for_migration_complete(from);

    pid = fork();
    if (!pid) {
        close(1);
        open("/dev/null", O_WRONLY);
        execl(python, python, ANALYZE_SCRIPT, "-f", file, NULL);
        g_assert_not_reached();
    }

    g_assert(waitpid(pid, &wstatus, 0) == pid);
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        g_test_message("Failed to analyze the migration stream");
        g_test_fail();
    }
    migrate_end(from, to, false);
    unlink(file);
}
#endif

#if 0
/* Currently upset on aarch64 TCG */
static void test_ignore_shared(void)
{
    g_autofree char *uri = g_strdup_printf("unix:%s/migsocket", tmpfs);
    QTestState *from, *to;

    if (migrate_start(&from, &to, uri, false, true, NULL, NULL)) {
        return;
    }

    migrate_ensure_non_converge(from);
    migrate_prepare_for_dirty_mem(from);

    migrate_set_capability(from, "x-ignore-shared", true);
    migrate_set_capability(to, "x-ignore-shared", true);

    /* Wait for the first serial output from the source */
    wait_for_serial("src_serial");

    migrate_qmp(from, to, uri, NULL, "{}");

    migrate_wait_for_dirty_mem(from, to);

    wait_for_stop(from, get_src());

    qtest_qmp_eventwait(to, "RESUME");

    wait_for_serial("dest_serial");
    wait_for_migration_complete(from);

    /* Check whether shared RAM has been really skipped */
    g_assert_cmpint(read_ram_property_int(from, "transferred"), <, 1024 * 1024);

    migrate_end(from, to, true);
}
#endif

static void *migrate_hook_start_mode_reboot(QTestState *from, QTestState *to)
{
    migrate_set_parameter_str(from, "mode", "cpr-reboot");
    migrate_set_parameter_str(to, "mode", "cpr-reboot");

    migrate_set_capability(from, "x-ignore-shared", true);
    migrate_set_capability(to, "x-ignore-shared", true);

    return NULL;
}

static void test_mode_reboot(void)
{
    g_autofree char *uri = g_strdup_printf("file:%s/%s", tmpfs,
                                           FILE_TEST_FILENAME);
    MigrateCommon args = {
        .start.use_shmem = true,
        .connect_uri = uri,
        .listen_uri = "defer",
        .start_hook = migrate_hook_start_mode_reboot,
    };

    test_file_common(&args, true);
}

static void do_test_validate_uuid(MigrateStart *args, bool should_fail)
{
    g_autofree char *uri = g_strdup_printf("unix:%s/migsocket", tmpfs);
    QTestState *from, *to;

    if (migrate_start(&from, &to, uri, args)) {
        return;
    }

    /*
     * UUID validation is at the begin of migration. So, the main process of
     * migration is not interesting for us here. Thus, set huge downtime for
     * very fast migration.
     */
    migrate_set_parameter_int(from, "downtime-limit", 1000000);
    migrate_set_capability(from, "validate-uuid", true);

    /* Wait for the first serial output from the source */
    wait_for_serial("src_serial");

    migrate_qmp(from, to, uri, NULL, "{}");

    if (should_fail) {
        qtest_set_expected_status(to, EXIT_FAILURE);
        wait_for_migration_fail(from, true);
    } else {
        wait_for_migration_complete(from);
    }

    migrate_end(from, to, false);
}

static void test_validate_uuid(void)
{
    MigrateStart args = {
        .opts_source = "-uuid 11111111-1111-1111-1111-111111111111",
        .opts_target = "-uuid 11111111-1111-1111-1111-111111111111",
    };

    do_test_validate_uuid(&args, false);
}

static void test_validate_uuid_error(void)
{
    MigrateStart args = {
        .opts_source = "-uuid 11111111-1111-1111-1111-111111111111",
        .opts_target = "-uuid 22222222-2222-2222-2222-222222222222",
        .hide_stderr = true,
    };

    do_test_validate_uuid(&args, true);
}

static void test_validate_uuid_src_not_set(void)
{
    MigrateStart args = {
        .opts_target = "-uuid 22222222-2222-2222-2222-222222222222",
        .hide_stderr = true,
    };

    do_test_validate_uuid(&args, false);
}

static void test_validate_uuid_dst_not_set(void)
{
    MigrateStart args = {
        .opts_source = "-uuid 11111111-1111-1111-1111-111111111111",
        .hide_stderr = true,
    };

    do_test_validate_uuid(&args, false);
}

static void do_test_validate_uri_channel(MigrateCommon *args)
{
    QTestState *from, *to;

    if (migrate_start(&from, &to, args->listen_uri, &args->start)) {
        return;
    }

    /* Wait for the first serial output from the source */
    wait_for_serial("src_serial");

    /*
     * 'uri' and 'channels' validation is checked even before the migration
     * starts.
     */
    migrate_qmp_fail(from, args->connect_uri, args->connect_channels, "{}");
    migrate_end(from, to, false);
}

static void test_validate_uri_channels_both_set(void)
{
    MigrateCommon args = {
        .start = {
            .hide_stderr = true,
        },
        .listen_uri = "defer",
        .connect_uri = "tcp:127.0.0.1:0",
        .connect_channels = ("[ { ""'channel-type': 'main',"
                             "    'addr': { 'transport': 'socket',"
                             "              'type': 'inet',"
                             "              'host': '127.0.0.1',"
                             "              'port': '0' } } ]"),
    };

    do_test_validate_uri_channel(&args);
}

static void test_validate_uri_channels_none_set(void)
{
    MigrateCommon args = {
        .start = {
            .hide_stderr = true,
        },
        .listen_uri = "defer",
    };

    do_test_validate_uri_channel(&args);
}

int main(int argc, char **argv)
{
    MigrationTestEnv *env;
    int ret;

    g_test_init(&argc, &argv, NULL);
    env = migration_get_env();
    module_call_init(MODULE_INIT_QOM);

    tmpfs = env->tmpfs;

    migration_test_add_tls(env);
    migration_test_add_compression(env);
    migration_test_add_postcopy(env);
    migration_test_add_file(env);
    migration_test_add_precopy(env);

    migration_test_add("/migration/bad_dest", test_baddest);
#ifndef _WIN32
    migration_test_add("/migration/analyze-script", test_analyze_script);
#endif

    /*
     * Our CI system has problems with shared memory.
     * Don't run this test until we find a workaround.
     */
    if (getenv("QEMU_TEST_FLAKY_TESTS")) {
        migration_test_add("/migration/mode/reboot", test_mode_reboot);
    }

    /* migration_test_add("/migration/ignore_shared", test_ignore_shared); */

    migration_test_add("/migration/validate_uuid", test_validate_uuid);
    migration_test_add("/migration/validate_uuid_error",
                       test_validate_uuid_error);
    migration_test_add("/migration/validate_uuid_src_not_set",
                       test_validate_uuid_src_not_set);
    migration_test_add("/migration/validate_uuid_dst_not_set",
                       test_validate_uuid_dst_not_set);
    migration_test_add("/migration/validate_uri/channels/both_set",
                       test_validate_uri_channels_both_set);
    migration_test_add("/migration/validate_uri/channels/none_set",
                       test_validate_uri_channels_none_set);

    ret = g_test_run();

    g_assert_cmpint(ret, ==, 0);

    tmpfs = NULL;
    ret = migration_env_clean(env);

    return ret;
}
