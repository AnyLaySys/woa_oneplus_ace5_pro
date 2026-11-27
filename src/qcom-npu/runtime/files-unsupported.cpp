#include "rpc.h"
#include "vendor/apps_std.h"
#include "vendor/AEEStdErr.h"
extern "C" {
int apps_std_freopen(apps_std_FILE, const char*, const char*, apps_std_FILE*) { return AEE_EUNSUPPORTED; }
int apps_std_fwrite(apps_std_FILE, const unsigned char*, int, int*, int*) { return AEE_EUNSUPPORTED; }
int apps_std_setenv(const char*, const char*, int) { return AEE_EUNSUPPORTED; }
int apps_std_unsetenv(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_fgets(apps_std_FILE, unsigned char*, int, int*) { return AEE_EUNSUPPORTED; }
int apps_std_fsync(apps_std_FILE) { return AEE_EUNSUPPORTED; }
int apps_std_fremove(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_fdopen_decrypt(apps_std_FILE, apps_std_FILE*) { return AEE_EUNSUPPORTED; }
int apps_std_opendir(const char*, apps_std_DIR*) { return AEE_EUNSUPPORTED; }
int apps_std_closedir(const apps_std_DIR*) { return AEE_EUNSUPPORTED; }
int apps_std_readdir(const apps_std_DIR*, apps_std_DIRENT*, int*) { return AEE_EUNSUPPORTED; }
int apps_std_mkdir(const char*, int) { return AEE_EUNSUPPORTED; }
int apps_std_rmdir(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_ftrunc(apps_std_FILE, int) { return AEE_EUNSUPPORTED; }
int apps_std_frename(const char*, const char*) { return AEE_EUNSUPPORTED; }
}
