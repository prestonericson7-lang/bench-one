// Encoder.h -- host stand-in: position comes from host_enc, which the test sets.
#pragma once
#include <Arduino.h>
extern long host_enc;
class Encoder {
 public:
  Encoder(int, int) {}
  long read() { return host_enc; }
  void write(long v) { host_enc = v; }
};
