#include "DynamicMultiPrint.h"

DynamicMultiPrint::DynamicMultiPrint()
    : _print1(nullptr), _print2(nullptr) {} // Initialize prints as null

void DynamicMultiPrint::setPrint1(Print* print) {
    _print1 = print;
}

void DynamicMultiPrint::setPrint2(Print* print) {
    _print2 = print;
}

size_t DynamicMultiPrint::write(uint8_t c) {
    size_t count1 = 0, count2 = 0;

    // Write to the first print if set
    if (_print1 != nullptr) {
        count1 = _print1->write(c);
    }

    // Write to the second print if set
    if (_print2 != nullptr) {
        count2 = _print2->write(c);
    }

    // Return the larger of the two counts
    return (count1 > count2) ? count1 : count2;
}
