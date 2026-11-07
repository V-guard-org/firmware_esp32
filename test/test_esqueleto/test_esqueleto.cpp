// Confere, na compilação, que o ambiente native segue as regras do código (proposta §4.8):
// C++17, sem exceções e sem RTTI. Os testes do núcleo chegam com os tickets seguintes.
#include <unity.h>

#ifdef __cpp_exceptions
#error "o native deve compilar sem exceções (native_cxx.py)"
#endif

#ifdef __GXX_RTTI
#error "o native deve compilar sem RTTI (native_cxx.py)"
#endif

static_assert(__cplusplus >= 201703L, "o native deve compilar em C++17 (platformio.ini)");

void setUp() {}

void tearDown() {}

// As regras são conferidas na compilação, acima. Este teste só faz o Unity rodar.
static void teste_compila_com_as_regras() { TEST_PASS(); }

int main() {
    UNITY_BEGIN();
    RUN_TEST(teste_compila_com_as_regras);
    return UNITY_END();
}
