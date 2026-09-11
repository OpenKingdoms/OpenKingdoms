/*
 * test_dataset.c -- Which game the mounted archives make up, and the
 * mount filter that lets a test leave the expansion's archives out.
 * Synthetic archives only, so it runs without game data.
 */

#include "test_framework.h"
#include "test_hpi_builder.h"
#include "tak_dataset.h"
#include "tak_hpi.h"
#include "tak_util.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define ds_mkdir(p) _mkdir(p)
#define ds_rmdir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define ds_mkdir(p) mkdir(p, 0755)
#define ds_rmdir(p) rmdir(p)
#endif

#define DS_DIR "test_dataset_tmp"

static const TestHPIEntry ds_base[] = {
    { "gamedata/sidedata.tdf", "[SIDE0]{name=ARAMON;}", 1000, 0 },
    { "camps/book of darien.tdf", "[HEADER]{}", 1000, 0 },
};
static const TestHPIEntry ds_ip_both[] = {
    { "camps/the iron plague.tdf", "[HEADER]{}", 2000, 0 },
    { "camps/ipalt.tdf", "[HEADER]{}", 2000, 0 },
};
static const TestHPIEntry ds_ip_campaign_only[] = {
    { "camps/the iron plague.tdf", "[HEADER]{}", 2000, 0 },
};
static const TestHPIEntry ds_ip_alt_only[] = {
    { "camps/ipalt.tdf", "[HEADER]{}", 2000, 0 },
};

static void ds_cleanup(void) {
    VFS_Shutdown();
    remove(DS_DIR "/data.hpi");
    remove(DS_DIR "/IPData.hpi");
    ds_rmdir(DS_DIR);
}

/* data.hpi always, IPData.hpi holding `ip` when given. */
static int ds_mount(const TestHPIEntry *ip, int ip_count) {
    ds_cleanup();
    ds_mkdir(DS_DIR);
    if (test_write_hpi(DS_DIR "/data.hpi", ds_base, 2) != 0) return -1;
    if (ip && test_write_hpi(DS_DIR "/IPData.hpi", ip, ip_count) != 0) return -1;
    return VFS_Init(DS_DIR, NULL);
}

static int ds_not_expansion(const char *file_name) {
    return tak_strnicmp(file_name, "ip", 2) != 0;
}

TEST(base_archives_are_not_iron_plague) {
    ASSERT_EQ_INT(0, ds_mount(NULL, 0));
    int ip = TAK_DataSet_HasIronPlague();
    ds_cleanup();
    ASSERT_EQ_INT(0, ip);
}

TEST(both_campaign_files_make_it_iron_plague) {
    ASSERT_EQ_INT(0, ds_mount(ds_ip_both, 2));
    int ip = TAK_DataSet_HasIronPlague();
    ds_cleanup();
    ASSERT_EQ_INT(1, ip);
}

TEST(one_campaign_file_is_not_enough) {
    ASSERT_EQ_INT(0, ds_mount(ds_ip_campaign_only, 1));
    int campaign_only = TAK_DataSet_HasIronPlague();
    ASSERT_EQ_INT(0, ds_mount(ds_ip_alt_only, 1));
    int alt_only = TAK_DataSet_HasIronPlague();
    ds_cleanup();
    ASSERT_EQ_INT(0, campaign_only);
    ASSERT_EQ_INT(0, alt_only);
}

TEST(pretend_no_expansion_turns_it_off) {
    ASSERT_EQ_INT(0, ds_mount(ds_ip_both, 2));
    TAK_DataSet_SetPretendNoExpansion(1);
    int pretending = TAK_DataSet_HasIronPlague();
    TAK_DataSet_SetPretendNoExpansion(0);
    int present = TAK_DataSet_HasIronPlague();
    ds_cleanup();
    ASSERT_EQ_INT(0, pretending);
    ASSERT_EQ_INT(1, present);
}

TEST(the_mount_filter_leaves_the_expansion_out) {
    VFS_SetMountFilter(ds_not_expansion);
    int rc = ds_mount(ds_ip_both, 2);
    VFS_SetMountFilter(NULL);
    int filtered_count = VFS_GetArchiveCount();
    int filtered_ip = TAK_DataSet_HasIronPlague();
    int filtered_alt = VFS_FileExists("camps/ipalt.tdf");
    VFS_Shutdown();
    int rc2 = VFS_Init(DS_DIR, NULL);
    int full_count = VFS_GetArchiveCount();
    int full_ip = TAK_DataSet_HasIronPlague();
    ds_cleanup();
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(1, filtered_count);
    ASSERT_EQ_INT(0, filtered_ip);
    ASSERT(filtered_alt != 0);
    ASSERT_EQ_INT(0, rc2);
    ASSERT_EQ_INT(2, full_count);
    ASSERT_EQ_INT(1, full_ip);
}

int main(void) {
    TEST_SUITE("Data set");
    RUN(base_archives_are_not_iron_plague);
    RUN(both_campaign_files_make_it_iron_plague);
    RUN(one_campaign_file_is_not_enough);
    RUN(pretend_no_expansion_turns_it_off);
    RUN(the_mount_filter_leaves_the_expansion_out);
    TEST_REPORT();
}
