#include "MultiStream.h"

MultiStream::MultiStream(Stream &stream1, Stream &stream2)
    : _stream1(stream1), _stream2(stream2) {}

int MultiStream::available() {
    return _stream1.available() + _stream2.available();
}

int MultiStream::read() {
    if (_stream1.available()) {
        return _stream1.read();
    }
    if (_stream2.available()) {
        return _stream2.read();
    }
    return -1;
}

int MultiStream::peek() {
    if (_stream1.available()) {
        return _stream1.peek();
    }
    if (_stream2.available()) {
        return _stream2.peek();
    }
    return -1;
}

size_t MultiStream::write(uint8_t c) {
    _stream1.write(c);
    return _stream2.write(c);
}