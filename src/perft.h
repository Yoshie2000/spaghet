#pragma once

#include "defs.h"
#include "movegen.h"

void perft_driver(const std::string &fen, uint8_t depth);

uint64_t perft(Board &board, uint8_t depth);

uint64_t perft_debug(Board &board, uint8_t depth, uint8_t start_depth);