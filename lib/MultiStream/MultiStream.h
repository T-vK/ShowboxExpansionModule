#ifndef MULTISTREAM_H
#define MULTISTREAM_H

#include <Arduino.h>

class MultiStream : public Stream {
  public:
    MultiStream(Stream &stream1, Stream &stream2);
    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual size_t write(uint8_t c) override;

  private:
    Stream &_stream1;
    Stream &_stream2;
};

#endif // MULTISTREAM_H