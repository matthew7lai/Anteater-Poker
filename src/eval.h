#ifndef EVAL_H
#define EVAL_H

#include "types.h"


typedef enum {
    HAND_HIGH_CARD      = 0,
    HAND_ONE_PAIR       = 1,
    HAND_TWO_PAIR       = 2,
    HAND_THREE_OF_KIND  = 3,
    HAND_STRAIGHT       = 4,
    HAND_FLUSH          = 5,
    HAND_FULL_HOUSE     = 6,
    HAND_FOUR_OF_KIND   = 7,
    HAND_STRAIGHT_FLUSH = 8,
    HAND_ROYAL_FLUSH    = 9
} HandRank;


typedef struct {
    int  rank;                        /**< @see HandRank.                    */
    int  tiebreak[BEST_HAND_SIZE];    /**< Ordered tiebreaker ranks.         */
    Card cards[BEST_HAND_SIZE];       /**< The chosen five cards.            */
} HandResult;


int evaluate_best_hand(const Card *pool, int pool_size,
                       Card best5_out[BEST_HAND_SIZE],
                       char hand_name_out[32]);


int compare_hands(int rank_a, const int tb_a[BEST_HAND_SIZE],
                  int rank_b, const int tb_b[BEST_HAND_SIZE]);

#endif /* EVAL_H */
