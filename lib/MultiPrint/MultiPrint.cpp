#include "MultiPrint.h"

MultiPrint::MultiPrint(Print &print1, Print &print2)
    : _print1(print1), _print2(print2) {}

size_t MultiPrint::write(uint8_t c) {
    _print1.write(c);
    return _print2.write(c);
}