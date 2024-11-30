#include "DynamicMultiPrint.h"

DynamicMultiPrint::DynamicMultiPrint()
    : _print1(nullptr), _print2(nullptr),
      _buffer1(false), _buffer2(false),
      _bufferSize1(0), _bufferSize2(0) {}

void DynamicMultiPrint::setPrint1(Print* print) {
    if (_print1 == nullptr && _bufferSize1 > 0) {
        // Flush any buffered data if the new print object is set
        flushBuffer(print, _bufferData1, _bufferSize1);
    }
    _print1 = print;
}

void DynamicMultiPrint::setPrint2(Print* print) {
    if (_print2 == nullptr && _bufferSize2 > 0) {
        // Flush any buffered data if the new print object is set
        flushBuffer(print, _bufferData2, _bufferSize2);
    }
    _print2 = print;
}

void DynamicMultiPrint::setBuffer1(bool enable) {
    _buffer1 = enable;
    if (!enable && _print1 != nullptr && _bufferSize1 > 0) {
        // Flush the buffer if disabled and a print object is available
        flushBuffer(_print1, _bufferData1, _bufferSize1);
    }
}

void DynamicMultiPrint::setBuffer2(bool enable) {
    _buffer2 = enable;
    if (!enable && _print2 != nullptr && _bufferSize2 > 0) {
        // Flush the buffer if disabled and a print object is available
        flushBuffer(_print2, _bufferData2, _bufferSize2);
    }
}

size_t DynamicMultiPrint::write(uint8_t c) {
    size_t count1 = 0;
    size_t count2 = 0;
    // Always write to buffer if enabled
    if (_buffer1) {
        addToBuffer(c, _bufferData1, _bufferSize1);
    } else if (_bufferSize1 > 0 && _print1 != nullptr) {
        // Flush buffered data if buffer is disabled and print is set
        flushBuffer(_print1, _bufferData1, _bufferSize1);
    } else {
        // Write directly to the print object if buffer is disabled
        if (_print1 != nullptr) {
            count1 = _print1->write(c);
        }
    }

    if (_buffer2) {
        addToBuffer(c, _bufferData2, _bufferSize2);
    } else if (_bufferSize2 > 0 && _print2 != nullptr) {
        // Flush buffered data if buffer is disabled and print is set
        flushBuffer(_print2, _bufferData2, _bufferSize2);
    } else {
        // Write directly to the print object if buffer is disabled
        if (_print2 != nullptr) {
            count2 = _print2->write(c);
        }
    }

    // Return the larger of the two counts
    return (count1 > count2) ? count1 : count2;
}

void DynamicMultiPrint::addToBuffer(uint8_t c, uint8_t* buffer, size_t& bufferSize) {
    if (bufferSize < MAX_BUFFER_SIZE) {
        // Add byte to buffer if there is space
        buffer[bufferSize++] = c;
    } else {
        // Buffer full, sacrifice oldest byte by shifting
        memmove(buffer, buffer + 1, MAX_BUFFER_SIZE - 1);
        buffer[MAX_BUFFER_SIZE - 1] = c;
    }
}

void DynamicMultiPrint::flushBuffer(Print* print, uint8_t* buffer, size_t& bufferSize) {
    if (print == nullptr || bufferSize == 0) return;

    // Write all buffered data to the print object
    print->write(buffer, bufferSize);
    bufferSize = 0; // Clear buffer after flushing
}
