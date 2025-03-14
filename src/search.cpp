#include "search.h"

int max_depth = 255;

// Searcher::Searcher() : board(Board(start_position))
// {
//     this->board = Board(start_position);
//     this->end_time = UINT64_MAX;
// }

Searcher::Searcher(Board &board, std::vector<Move> &move_list, TranspositionTable &transposition_table, ThreadData &thread_data, uint32_t age) : board(board), transposition_table(transposition_table), thread_data(thread_data)
{
    // reserves enough space so we don't have to resize
    game_history.reserve(300 + MAX_PLY);

    game_history.push_back(board.hash);

    for (Move m : move_list)
    {
        board.make_move(m);
        // if (count_bits(board.bitboard(WHITE_KING)) == 2)
        //     board.print();
        game_history.push_back(board.hash);
    }

    nodes_spent_table.fill(0);

    thread_data.search_stack[4].board = board;

    this->age = age;
    this->transposition_table = transposition_table;
}

// Searcher::Searcher(Board &board, std::vector<Move> &move_list, std::vector<SearchStack> &search_stack, TranspositionTable &transposition_table, QuietHistory &history, ContinuationHistory &conthist, uint32_t age, uint64_t end_time) : board(board), transposition_table(transposition_table), history(history), conthist(conthist), search_stack(search_stack)
// {
//     // reserves enough space so we don't have to resize
//     game_history.reserve(300 + MAX_PLY);

//     game_history.push_back(board.hash);

//     for (Move m : move_list)
//     {
//         board.make_move(m);
//         // if (count_bits(board.bitboard(WHITE_KING)) == 2)
//         //     board.print();
//         game_history.push_back(board.hash);
//     }

//     search_stack[4].board = board;

//     nodes_spent_table.fill(0);

//     this->board = board;
//     this->age = age;
//     this->transposition_table = transposition_table;
//     this->history = history;
//     this->max_stop_time = end_time;
//     this->optimum_stop_time = end_time;
//     this->max_stop_time_duration = end_time - get_time();
//     this->optimum_stop_time_duration = end_time - get_time();
// }
// Searcher::Searcher(Board &board, std::vector<Move> &move_list, uint64_t end_time, uint8_t max_depth)
//     : board(board)
// {

//     for (Move m : move_list)
//     {
//         board.make_move(m);
//     }

//     this->board = board;
//     this->end_time = end_time;
//     this->max_depth = max_depth;
// }

// bool Searcher::is_checkmate(Board &board)
// {
//     return false;
// }

bool Searcher::twofold(Board &board)
{
    // in here, the board's hash is already added into the game_history
    uint64_t hash = board.hash;

    // index of the last element of the array
    size_t last_element_index = game_history.size() - 1;

    int threefold_max_it = std::min((size_t)board.fifty_move_counter, last_element_index);

    for (int i = 4; i <= threefold_max_it; i += 2)
    {
        if (hash == game_history[last_element_index - i])
            return true;
    }

    // did not find a matching hash
    return false;
}

void Searcher::scale_time(int best_move_stability_factor)
{
    constexpr double best_move_scale[5] = {2.43, 1.35, 1.09, 0.88, 0.68};
    const Move best_move = thread_data.search_stack[4].pv.moves[0];
    const double best_move_nodes_fraction = static_cast<double>(nodes_spent_table[best_move.from_to()]) / static_cast<double>(nodes);
    const double node_scaling_factor = (1.52 - best_move_nodes_fraction) * 1.74;
    const double best_move_scaling_factor = best_move_scale[best_move_stability_factor];
    // scal9e the time based on how many nodes we spent ond how the best move changed
    optimum_stop_time = std::min<uint64_t>(start_time + optimum_stop_time_duration * node_scaling_factor * best_move_scaling_factor, max_stop_time);
}

void Searcher::update_conthist(SearchStack *ss, MoveList &quiet_moves, Move fail_high_move, int depth)
{
    int ply = ss->ply;

    // // updates followup move history
    // if (ply >= 2 && !(ss - 2)->null_moved)
    //     thread_data.conthist.update(ss->board, quiet_moves, fail_high_move, (ss - 2)->board, (ss - 2)->move_played, depth);

    // updates counter move history
    if (ply >= 1 && !(ss - 1)->null_moved)
        thread_data.conthist.update(ss->board, quiet_moves, fail_high_move, (ss - 1)->board, (ss - 1)->move_played, depth);
}

template <bool inPV>
int Searcher::quiescence_search(int alpha, int beta, SearchStack *ss)
{
    // return evaluate(board);

    if (stopped)
        return 0;

    if (ss->ply > seldepth)
        seldepth = ss->ply;

    ++nodes;
    if (!(nodes & 4095))
        if (get_time() >= max_stop_time)
        {
            stopped = true;
            return 0;
        }

    Board &board = ss->board;

    // we check if the TT has seen this before
    TT_Entry &entry = transposition_table.probe(board);

    // tt cutoff
    // if the entry matches, we can use the score, and the depth is the same or greater, we can just cut the search short
    if (!inPV && entry.hash == board.hash && entry.can_use_score(alpha, beta))
    {
        return entry.usable_score(ss->ply);
    }

    // creates a baseline
    int stand_pat = evaluate(board);

    if (ss->ply >= MAX_PLY - 1)
        return stand_pat;

    if (stand_pat >= beta)
        return stand_pat; // fail soft

    if (alpha < stand_pat)
        alpha = stand_pat;

    int best_eval = stand_pat;
    // int capture_moves = 0;
    MoveList move_list;
    generate_capture_moves(board, move_list);

    // creates a "garbage" move so that when we read from the TT we don't accidentally order a random move first during scoring
    Move best_move(a8, a8, MOVE_FLAG::QUIET_MOVE);

    const int original_alpha = alpha;

    // scores moves to order them
    MovePicker move_picker(move_list);
    move_picker.score(board, ss, transposition_table, thread_data.main_history, thread_data.conthist, ss->killers, -107);

    while (move_picker.has_next())
    {
        Board copy = board;
        OrderedMove curr_move = move_picker.next_move();

        copy.make_move(curr_move);

        if (!copy.was_legal())
            continue;

        // do we need to check for checkmate in qsearch?
        // if (is_checkmate(copy))
        // {
        //     return -50000 + depth
        // }

        // qsearch SEE pruning
        // since we only generate capture moves, if the score of the move is negative, that means it did not pass the SEE threshold, so we can just stop the loop
        // since everything after it will also not pass the SEE threshold
        if (curr_move.score < 0)
            break;

        (ss + 1)->board = copy;
        (ss)->move_played = curr_move;

        int current_eval = -quiescence_search<inPV>(-beta, -alpha, ss + 1);

        if (stopped)
            return 0;

        if (current_eval > best_eval)
        {
            best_eval = current_eval;
            best_move = curr_move;

            // ++capture_moves;

            if (current_eval > alpha)
            {
                alpha = current_eval;
                if (alpha >= beta)
                {
                    break; // fail soft
                }
            }
        }
    }

    // add to TT
    uint8_t bound_flag = BOUND::EXACT;

    if (alpha >= beta)
    {
        // beta cutoff, fail high
        bound_flag = BOUND::FAIL_HIGH;
    }
    else if (alpha <= original_alpha)
    {
        // failed to raise alpha, fail low
        bound_flag = BOUND::FAIL_LOW;
    }
    transposition_table.insert(board, best_move, best_eval, 0, ss->ply, age, bound_flag);

    // TODO: add check moves
    return best_eval;
}

template <bool inPV>
int Searcher::negamax(int alpha, int beta, int depth, SearchStack *ss)
{
    ++nodes;

    if (stopped)
        return 0;

    if (depth == 0 && ss->ply > seldepth)
        seldepth = ss->ply;

    if (!(nodes & 4095))
        if (get_time() >= max_stop_time)
        {
            stopped = true;
            return 0;
        }

    bool in_root = ss->ply <= 0;

    Board &board = ss->board;

    if (inPV)
    {
        ss->pv.clear();
        (ss + 1)->pv.clear();
    }

    // cut the search short if there's a draw
    // if it's a draw at the root node, we'll play a null move
    if (!in_root && board.fifty_move_counter >= 100)
        return 0;

    // if there's a threefold draw
    if (!in_root && twofold(board))
    {
        // std::cout << "threefold repetition" << "\n";
        return 0;
    }

    // bool inPV = beta - alpha > 1;

    // we check if the TT has seen this before
    TT_Entry &entry = transposition_table.probe(board);

    // tt cutoff
    // if the entry matches, we can use the score, and the depth is the same or greater, we can just cut the search short
    if (!inPV && entry.hash == board.hash && entry.can_use_score(alpha, beta) && entry.depth >= depth)
    {
        return entry.usable_score(ss->ply);
    }

    if (depth <= 0)
        return quiescence_search<inPV>(alpha, beta, ss);

    int static_eval = evaluate(board);

    // apply reverse futility pruning
    if (!inPV && !board.is_in_check() && depth <= DEPTH_MARGIN && static_eval - depth * MARGIN >= beta)
        return static_eval;

    // bailout
    if (ss->ply >= MAX_PLY - 1)
        return static_eval;

    // applies null move pruning
    if (!(ss - 1)->null_moved && !inPV && !board.is_in_check() && !board.only_pawns(board.side_to_move) && static_eval >= beta)
    {

        Board copy = board;
        copy.make_null_move();

        // to help detect threefold in nmp
        game_history.push_back(copy.hash);

        // make sure that immediately after we finishd null moving we set the search stack to false, helps with persistent search stack later down the line
        ss->null_moved = true;
        (ss + 1)->board = copy;

        int null_move_score = -negamax<nonPV>(-beta, -beta + 1, depth - NULL_MOVE_DEPTH_REDUCTION, ss + 1);

        ss->null_moved = false;

        if (stopped)
            return 0;

        game_history.pop_back();

        if (null_move_score >= beta)
            return null_move_score;
    }

    MoveList move_list;
    MoveList quiet_moves;

    generate_moves(board, move_list);

    // scores moves to order them
    MovePicker move_picker(move_list);
    move_picker.score(board, ss, transposition_table, thread_data.main_history, thread_data.conthist, ss->killers, -107);

    const int original_alpha = alpha;

    // get pvs here
    int best_eval = -INF - 1;
    Move best_move;
    bool is_quiet;

    const int futility_margin = 150 + 100 * depth;

    int nodes_before_search = nodes;

    while (move_picker.has_next())
    {
        Board copy = board;
        Move curr_move = move_picker.next_move();
        copy.make_move(curr_move);

        if (!copy.was_legal())
            continue;

        move_picker.update_legal_moves();

        is_quiet = curr_move.is_quiet();

        if (!in_root && best_eval > MIN_MATE_SCORE)
        {
            // applies late move pruning
            if (is_quiet && move_picker.moves_seen() >= 3 + depth * depth)
            {
                move_picker.skip_quiets();
                continue;
            }

            // applies pvs see pruning
            const int see_threshold = is_quiet ? -80 * depth : -30 * depth * depth;

            if (depth <= 8 && move_picker.moves_seen() > 0 && !SEE(board, curr_move, see_threshold))
                continue;

            // applies futility pruning
            if (depth <= 8 && !board.is_in_check() && is_quiet && static_eval + futility_margin < alpha)
            {
                move_picker.skip_quiets();
                continue;
            }
        }

        // now that we haven't pruned anything, we can update the search stack
        (ss + 1)->board = copy;
        (ss)->move_played = curr_move;

        if (is_quiet)
            quiet_moves.insert(curr_move);

        int new_depth = depth - 1;
        int extension = 0;

        // extensions
        if (copy.is_in_check())
            extension += 1;

        new_depth += extension;

        int current_eval;

        // don't do pvs on the first node
        if (move_picker.moves_seen() == 0)
        {
            // we can check for threefold repetition later, updates the state though
            game_history.push_back(copy.hash);

            current_eval = -negamax<inPV>(-beta, -alpha, new_depth, ss + 1);

            if (stopped)
                return 0;

            // stopped searching that line, so we can get rid of the hash
            game_history.pop_back();
        }
        else
        {
            // applies the late move reduction
            if (move_picker.moves_seen() > 1)
            {
                if (is_quiet)
                    new_depth -= lmr_reduction_quiet(depth, move_picker.moves_seen());
                // noisy move
                else
                    new_depth -= lmr_reduction_captures_promotions(depth, move_picker.moves_seen());
            }

            // we can check for threefold repetition later, updates the state though
            game_history.push_back(copy.hash);

            // null windows search, basically checking if if returns alpha or alpha + 1 to indicate if there's a better move
            current_eval = -negamax<nonPV>(-alpha - 1, -alpha, new_depth, ss + 1);

            if (stopped)
                return 0;

            // stopped searching that line, so we can get rid of the hash
            game_history.pop_back();

            // if this node raises alpha that means that we should investigate a bit more with a full length search, but still null-window
            // if this one fails high, using PVS we assume that it is a PV-node, so we re-search with a full window
            if (current_eval > alpha)
            {
                game_history.push_back(copy.hash);

                current_eval = -negamax<nonPV>(-alpha - 1, -alpha, depth - 1, ss + 1);

                if (stopped)
                    return 0;

                game_history.pop_back();

                // pvs implementation, if we don 't have a fail low from that search, that means that our previous move wasn't our best move,
                // so we'll assume that this node is the pv move, and then do a full window search.
                if (current_eval > alpha && inPV)
                {
                    game_history.push_back(copy.hash);

                    current_eval = -negamax<PV>(-beta, -alpha, depth - 1, ss + 1);

                    if (stopped)
                        return 0;

                    // stopped searching that line, so we can get rid of the hash
                    game_history.pop_back();
                }
            }
        }

        move_picker.update_moves_seen();

        if (in_root)
        {
            nodes_spent_table[curr_move.from_to()] += nodes - nodes_before_search;
            nodes_before_search = nodes;
        }

        if (stopped)
            return 0;

        // fail soft framework
        if (current_eval > best_eval)
        {
            best_eval = current_eval;
            best_move = curr_move;

            if (current_eval > alpha)
            {
                alpha = current_eval;
                best_move = curr_move;

                // logic to update the pv when we have a new best_move
                if (inPV)
                {
                    ss->pv.clear();
                    ss->pv.insert(best_move);
                    ss->pv.copy_over((ss + 1)->pv);
                }

                // fail high
                if (alpha >= beta)
                {

                    // std::cout << (int)curr_move.move_flag() << "\n";
                    // we update the history table if it's not a capture
                    if (is_quiet)
                    {
                        update_conthist(ss, quiet_moves, curr_move, depth);
                        thread_data.main_history.update(quiet_moves, curr_move, depth, board.side_to_move);
                        ss->killers.insert(curr_move);
                    }
                    break;
                }
            }
        }
    }

    // uncomment this if it doesn't work
    // write the best move down at the current depth

    if (move_picker.moves_seen() == 0)
    {
        if (board.is_in_check())
        {
            // prioritize faster mates
            return -MATE + ss->ply;
        }
        else
        {
            return 0;
        }
    }

    // add to TT
    uint8_t bound_flag = BOUND::EXACT;

    if (alpha >= beta)
    {
        // beta cutoff, fail high
        bound_flag = BOUND::FAIL_HIGH;
    }
    else if (alpha <= original_alpha)
    {
        // failed to raise alpha, fail low
        bound_flag = BOUND::FAIL_LOW;
    }
    if (best_eval != (-INF - 1))
        transposition_table.insert(board, best_move, best_eval, depth, ss->ply, age, bound_flag);

    return best_eval;
}

void Searcher::search()
{
    int best_score = -INF;
    Move previous_best_move(a8, a8, 0);
    Move best_move(a8, a8, 0);
    int best_move_stability_factor = 0;
    uint64_t time_elapsed;
    int alpha = -INF;
    int beta = INF;
    int search_again_counter = 0;

    Board board = thread_data.search_stack[4].board;

    // this->start_time = get_time();
    nodes = 0;

    // generates a legal move in that position in case that we didn't finish depth one
    MoveList move_list;
    generate_moves(board, move_list);

    for (int i = 0; i < move_list.size(); ++i)
    {
        Board copy = board;
        copy.make_move(move_list.moves[i]);

        if (copy.was_legal())
        {
            best_move = move_list.moves[i];
            break;
        }
    }

    for (int root_depth = 1; root_depth <= max_depth; ++root_depth)
    {
        this->curr_depth = root_depth;
        this->seldepth = 0;

        // STOCKFISH IMPLEMENTATION OF ASPIRATION WINDOWS

        // stockfish uses 9, let's try that later
        int delta = 9 + average_score * average_score / 10182;
        // int delta = 25;

        // alpha = -INF;
        // beta = INF;

        // if (root_depth > 5)
        // {
        alpha = std::max<int>(best_score - delta, -INF);
        beta = std::min<int>(best_score + delta, INF);
        // }

        // start with a small aspiration window and, in case of a fail high/low, re-search with a bigger window until we don't fail high/low anymore
        int failed_high_count = 0;

        while (true)
        {

            // missing the search again counter but it's always 0?
            int adjusted_depth = std::max(1, root_depth - failed_high_count);
            int root_delta = beta - alpha;
            // we start at 4 beacuse of conthist
            best_score = negamax<PV>(alpha, beta, adjusted_depth, &thread_data.search_stack[4]);

            if (stopped)
                break;

            if (best_score <= alpha)
            {
                beta = (alpha + beta) / 2;
                alpha = std::max<int>(best_score - delta, -INF);
            }
            else if (best_score >= beta)
            {
                beta = std::min<int>(best_score + delta, INF);
                ++failed_high_count;
            }
            else
                break;

            delta += delta / 3;
        }

        // std::cout << static_cast<int>(aspiration_adjustments) << " " << alpha << " " << beta << "\n";

        if (stopped)
            break;

        best_move = thread_data.search_stack[4].pv.moves[0];

        // clears the pv before starting the new search
        // for (int i = 0; i < MAX_PLY; ++i)
        //     std::cout << static_cast<int>(pv[i].size()) << " ";

        time_elapsed = std::max(get_time() - start_time, (uint64_t)1);

        if (is_mate_score(best_score))
            std::cout << "info depth " << static_cast<int>(root_depth) << " seldepth " << seldepth << " score mate " << mate_score_to_moves(best_score) << " nodes " << nodes << " time " << time_elapsed << " nps " << (uint64_t)((double)nodes / time_elapsed * 1000) << " pv " << thread_data.search_stack[4].pv.to_string() << " "
                      << std::endl;
        else
            std::cout << "info depth " << static_cast<int>(root_depth) << " seldepth " << seldepth << " score cp " << best_score << " nodes " << nodes << " time " << time_elapsed << " nps " << (uint64_t)((double)nodes / time_elapsed * 1000) << " pv " << thread_data.search_stack[4].pv.to_string() << " "
                      << std::endl;

        if (best_move == previous_best_move)
        {
            best_move_stability_factor = std::min(best_move_stability_factor + 1, 4);
        }
        else
        {
            best_move_stability_factor = 0;
            previous_best_move = best_move;
        }

        if (root_depth > 7 && time_set)
        {
            scale_time(best_move_stability_factor);
        }

        if (get_time() > optimum_stop_time)
            break;

        average_score = average_score != -INF ? (2 * best_score + average_score) / 3 : best_score;
    }

    // printf("bestmove %s\n", best_move.to_string().c_str());
    std::cout << "bestmove " << best_move.to_string() << " " << std::endl;
}
