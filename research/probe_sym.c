#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *libs[] = {
        "/usr/lib/libMemAdapter.so",
        "/usr/lib/libVE.so",
        "/usr/lib/libcdc_base.so",
    };
    const char *syms[] = {
        "MemAdapterGetOpsS", "SecureMemAdapterGetOpsS",
        "GetVeOpsS", "VeInitialize", "GetSbmInterface",
    };

    for (int i = 0; i < 3; i++) {
        void *h = dlopen(libs[i], RTLD_NOW);
        printf("== %s -> %p", libs[i], h);
        if (!h) { printf("  (%s)\n", dlerror()); continue; }
        printf("\n");
        for (int j = 0; j < 5; j++) {
            void *p = dlsym(h, syms[j]);
            if (p) printf("   %s = %p\n", syms[j], p);
        }
    }

    /* 再试从全局作用域找 */
    for (int j = 0; j < 5; j++) {
        void *p = dlsym(RTLD_DEFAULT, syms[j]);
        printf("default %s = %p\n", syms[j], p);
    }
    return 0;
}
