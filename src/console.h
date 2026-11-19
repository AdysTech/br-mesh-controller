#pragma once
// ============================================================================
//  Console — the serial console driver.
//
//  A thin command parser over a MeshController instance. Knows the wire protocol for the
//  console text only; every action it does delegates to MeshController. The bridge's HTTP
//  and HA drivers are its peers — the same MeshController, different front ends.
// ============================================================================
#include <Arduino.h>
#include <string>

class MeshController;

class Console {
 public:
  explicit Console(MeshController& mesh) : mesh_(mesh) {}

  /// Print the ready/help banner (call once at the end of setup()).
  void banner();
  /// Read + dispatch any pending console input. Call from loop().
  void poll();

 private:
  void handle(const char* line);

  MeshController& mesh_;
  int target_ = -1;             // 'sel' target light number (-1 = first light)
  char buf_[128];
  int len_ = 0;
};
