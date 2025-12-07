#include "utils.h"
namespace utils
{

bool is_power_of_two(uint64_t n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

}  // namespace utils