// cdrom.c ядра PCSX-ReARMed в нашей единице трансляции: исходник подмодуля не
// меняем, но получаем доступ к статическому состоянию привода (cdr.StatP) —
// по нему видно, что игра остановила мотор и ждёт смены диска.
#include "../third_party/pcsx_rearmed/libpcsxcore/cdrom.c"

int emu_cdr_stat(void)
{
    return cdr.StatP;
}
