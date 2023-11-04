#include "fw.h"

int main(void) {
    fwState *fws = fwStateNew("echo hello", 256, -1);
    fwAddFile(fws, "./sample-files/ex.txt");
    fwLoopMain(fws);
}
