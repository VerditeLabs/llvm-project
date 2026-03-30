// Test file for the Performance Sanitizer
// Compile with: clang -fperf-suggest -O2 test-perf-san.cpp -o /dev/null
//
// This file contains intentional performance anti-patterns that the
// performance sanitizer should flag with suggestions.

#include <cstddef>
#include <cstdint>

// === AST-Level Checks ===

// 1. Function that could be constexpr
int square(int x) {         // Should suggest: could be constexpr
  return x * x;
}

int factorial(int n) {      // Should suggest: could be constexpr
  if (n <= 1) return 1;
  return n * factorial(n - 1);
}

// 2. Large struct passed by value (should be const ref)
struct BigData {
  double values[32];  // 256 bytes
  int metadata[8];
};

double sumBigData(BigData data) {  // Should suggest: pass by const ref
  double sum = 0;
  for (int i = 0; i < 32; ++i)
    sum += data.values[i];
  return sum;
}

void processBigData(BigData a, BigData b) {  // Should suggest: pass by ref
  for (int i = 0; i < 32; ++i)
    a.values[i] += b.values[i];
}

// 3. Loop with non-constant bound (function call)
int getSize();
void processArray(int* arr) {
  for (int i = 0; i < getSize(); ++i) {  // Should suggest: fixed bound
    arr[i] *= 2;
  }
}

// 4. Loop with parameter-dependent bound
void fillArray(int* arr, int n) {
  for (int i = 0; i < n; ++i) {  // Should suggest: fixed bound or assume
    arr[i] = i;
  }
}

// 5. Virtual call in loop
struct Shape {
  virtual double area() const = 0;
  virtual ~Shape() = default;
};

double totalArea(Shape** shapes, int n) {
  double total = 0;
  for (int i = 0; i < n; ++i) {
    total += shapes[i]->area();  // Should suggest: virtual in loop
  }
  return total;
}

// 6. Move constructor without noexcept
struct Buffer {
  int* data;
  size_t size;

  Buffer(int* d, size_t s) : data(d), size(s) {}

  Buffer(Buffer&& other) : data(other.data), size(other.size) {
    // Should suggest: mark noexcept for better container behavior
    other.data = nullptr;
    other.size = 0;
  }

  Buffer& operator=(Buffer&& other) {
    // Should suggest: mark noexcept
    if (this != &other) {
      delete[] data;
      data = other.data;
      size = other.size;
      other.data = nullptr;
      other.size = 0;
    }
    return *this;
  }

  ~Buffer() { delete[] data; }
};

// === IR-Level Checks ===

// 7. Function that could be pure/const
int computeValue(int a, int b) {
  return a * a + b * b + 2 * a * b;
}

// 8. Large function that won't inline
int bigFunction(int x) {
  int result = x;
  // Generate lots of computation to exceed inline threshold
  result = result * 3 + 7;   result = result ^ (result >> 2);
  result = result * 5 + 11;  result = result ^ (result >> 3);
  result = result * 7 + 13;  result = result ^ (result >> 4);
  result = result * 11 + 17; result = result ^ (result >> 5);
  result = result * 13 + 19; result = result ^ (result >> 6);
  result = result * 17 + 23; result = result ^ (result >> 7);
  result = result * 19 + 29; result = result ^ (result >> 8);
  result = result * 23 + 31; result = result ^ (result >> 9);
  result = result * 29 + 37; result = result ^ (result >> 10);
  result = result * 31 + 41; result = result ^ (result >> 11);
  result = result * 37 + 43; result = result ^ (result >> 12);
  result = result * 41 + 47; result = result ^ (result >> 13);
  if (result > 0) {
    result = result * 3 + 7;   result = result ^ (result >> 2);
    result = result * 5 + 11;  result = result ^ (result >> 3);
    result = result * 7 + 13;  result = result ^ (result >> 4);
    result = result * 11 + 17; result = result ^ (result >> 5);
    result = result * 13 + 19; result = result ^ (result >> 6);
    result = result * 17 + 23; result = result ^ (result >> 7);
    result = result * 19 + 29; result = result ^ (result >> 8);
    result = result * 23 + 31; result = result ^ (result >> 9);
    result = result * 29 + 37; result = result ^ (result >> 10);
    result = result * 31 + 41; result = result ^ (result >> 11);
    result = result * 37 + 43; result = result ^ (result >> 12);
    result = result * 41 + 47; result = result ^ (result >> 13);
  }
  return result;
}

// 9. Pointer aliasing preventing vectorization
void addArrays(float* __restrict out, const float* a, const float* b, int n) {
  // This one uses __restrict so should be OK
  for (int i = 0; i < n; ++i)
    out[i] = a[i] + b[i];
}

void addArraysAliased(float* out, float* a, float* b, int n) {
  // This one has potential aliasing - should flag
  for (int i = 0; i < n; ++i)
    out[i] = a[i] + b[i];
}

int main() {
  // Use the functions so they're not optimized away
  int s = square(5);
  int f = factorial(6);

  BigData bd{};
  double sum = sumBigData(bd);

  int arr[100];
  fillArray(arr, 100);

  Buffer buf1{new int[10], 10};
  Buffer buf2 = static_cast<Buffer&&>(buf1);

  int v = computeValue(3, 4);
  int big = bigFunction(42);

  float fa[256], fb[256], fc[256];
  addArraysAliased(fa, fb, fc, 256);

  return s + f + static_cast<int>(sum) + v + big;
}
