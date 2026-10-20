/* Signature matches the initial mapped-pointer-only executor ABI. */
void nd_test_kernel(int *input, int *output) { *output = *input + 7; }
