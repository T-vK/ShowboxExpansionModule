#ifndef DYNAMIC_MULTIPRINT_H
#define DYNAMIC_MULTIPRINT_H

#include <Arduino.h>

class DynamicMultiPrint : public Print {
  public:
    DynamicMultiPrint(); // Default constructor with no prints set

    void setPrint1(Print* print); // Dynamically set Print1
    void setPrint2(Print* print); // Dynamically set Print2

    void setBuffer1(bool enable); 
    void setBuffer2(bool enable);

    virtual size_t write(uint8_t c) override;

  private:
    Print* _print1 = nullptr; // Pointer to Print1
    Print* _print2 = nullptr; // Pointer to Print2

    bool _buffer1; // Enable buffer for _print1
    bool _buffer2; // Enable buffer for _print2

    static const size_t MAX_BUFFER_SIZE = 8192;
    uint8_t _bufferData1[MAX_BUFFER_SIZE];
    uint8_t _bufferData2[MAX_BUFFER_SIZE];

    size_t _bufferSize1; // Current size of the buffer for _print1
    size_t _bufferSize2; // Current size of the buffer for _print2;

    void addToBuffer(uint8_t c, uint8_t* buffer, size_t& bufferSize);
    void flushBuffer(Print* print, uint8_t* buffer, size_t& bufferSize);
};

#endif // DYNAMIC_MULTIPRINT_H
