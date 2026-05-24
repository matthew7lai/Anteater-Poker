/*
 * eval.h  —  Anteater Poker hand evaluator (Team 23, EECS 22L)
 */
#ifndef EVAL_H
#define EVAL_H

#include "types.h"

/* Evaluate best 5-card hand from a pool of cards (handles wildcard).
 * Returns hand rank (0=High Card .. 9=Royal Flush).
 * best5_out[] receives the 5 cards chosen.
 * hand_name_out receives a string like "Full House".           */
int evaluate_best_hand(Card *pool, int pool_size,
                       Card best5_out[5],
                       char hand_name_out[32]);

/* Compare two evaluated hands; returns >0 if a wins, <0 if b wins, 0 tie */
int compare_hands(int rank_a, int tb_a[5],
                  int rank_b, int tb_b[5]);

#endif
