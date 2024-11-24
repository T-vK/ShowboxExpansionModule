#ifndef MULTIPRINT_H
#define MULTIPRINT_H

#include <Arduino.h>

class MultiPrint : public Print {
  public:
    MultiPrint(Print &print1, Print &print2);
    virtual size_t write(uint8_t c) override;

  private:
    Print &_print1;
    Print &_print2;
};

#endif // MULTIPRINT_H