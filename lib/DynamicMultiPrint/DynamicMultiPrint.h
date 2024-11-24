#ifndef DYNAMIC_MULTIPRINT_H
#define DYNAMIC_MULTIPRINT_H

#include <Arduino.h>

class DynamicMultiPrint : public Print {
  public:
    DynamicMultiPrint(); // Default constructor with no prints set

    void setPrint1(Print* print); // Dynamically set Print1
    void setPrint2(Print* print); // Dynamically set Print2

    virtual size_t write(uint8_t c) override;

  private:
    Print* _print1 = nullptr; // Pointer to Print1
    Print* _print2 = nullptr; // Pointer to Print2
};

#endif // DYNAMIC_MULTIPRINT_H
