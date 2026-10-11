// минимальная замена deps/libretro-common/features/features_cpu.c:
// от него pcsxr-threads.c нужна только cpu_features_get_core_amount,
// остальное тянет несовместимые с bionic compat-обёртки
#include <unistd.h>

int cpu_features_get_core_amount(void)
{
    return (int)sysconf(_SC_NPROCESSORS_CONF);
}
