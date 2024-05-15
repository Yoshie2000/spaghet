#include "history.h"

QuietHistory::QuietHistory()
{
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j)
            butterfly_table[i][j] = 0;
}

void QuietHistory::clear()
{
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j)
            butterfly_table[i][j] = 0;
}

void QuietHistory::insert(Move move, int depth)
{
    uint8_t from_square = move.from_square();
    uint8_t to_square = move.to_square();

    butterfly_table[from_square][to_square] = std::min(butterfly_table[from_square][to_square] + depth * depth, MAX_HISTORY);
}

uint16_t QuietHistory::move_value(Move move)
{
    return butterfly_table[move.from_square()][move.to_square()];
}