#include "fw.h"

int main(void) {
    fwState *fws = fwStateNew("python3 ./example.py", 256, -1);
    fwAddFile(fws, "./example.py");
    fwLoopMain(fws);
}
