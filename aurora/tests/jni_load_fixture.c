static int entered, release, calls;
static void *loaded_vm;

int fixture_entered(void) { return __atomic_load_n(&entered, __ATOMIC_SEQ_CST); }
int fixture_calls(void) { return __atomic_load_n(&calls, __ATOMIC_SEQ_CST); }
void *fixture_vm(void) { return loaded_vm; }
void fixture_release(void) { __atomic_store_n(&release, 1, __ATOMIC_SEQ_CST); }

int JNI_OnLoad(void *vm, void *reserved)
{
    (void)reserved;
    loaded_vm = vm;
    __atomic_add_fetch(&calls, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&entered, 1, __ATOMIC_SEQ_CST);
#ifndef JNI_LOAD_FAILURE
    while (!__atomic_load_n(&release, __ATOMIC_SEQ_CST)) {}
    return 0x10006;
#else
    return -1;
#endif
}
