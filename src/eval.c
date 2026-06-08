#include <string.h>
#include <stdio.h>

#include "eval.h"
#include "types.h"

/* Upper bound on the working pool: 2 hole + 5 community + 1 resolved wild,
 * with headroom. Used for fixed-size local buffers. */
#define MAX_POOL 16

/* ------------------------------------------------------------------------- */
/* Small local helpers                                                       */
/* ------------------------------------------------------------------------- */

/**
 * @brief Sort five ranks in place, descending.
 *
 * A plain selection sort: clearest choice for a fixed five-element array.
 */
static void sort_ranks_desc(int ranks[BEST_HAND_SIZE]) {
    for (int i = 0; i < BEST_HAND_SIZE - 1; i++) {
        for (int j = i + 1; j < BEST_HAND_SIZE; j++) {
            if (ranks[j] > ranks[i]) {
                int tmp  = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
    }
}


static void build_tiebreak(const int counts[NUM_RANKS], int tb[BEST_HAND_SIZE]) {
    int n = 0;
    for (int freq = 4; freq >= 1; freq--) {
        for (int r = NUM_RANKS - 1; r >= 0; r--) {
            if (counts[r] == freq) {
                for (int k = 0; k < freq && n < BEST_HAND_SIZE; k++) {
                    tb[n++] = r;
                }
            }
        }
    }
    while (n < BEST_HAND_SIZE) {
        tb[n++] = -1;
    }
}

/**
 * @brief Return non-zero if tiebreaker list @p a ranks strictly above @p b.
 */
static int tiebreak_beats(const int a[BEST_HAND_SIZE], const int b[BEST_HAND_SIZE]) {
    for (int i = 0; i < BEST_HAND_SIZE; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Five-card scoring                                                         */
/* ------------------------------------------------------------------------- */


static int score_five(const Card hand[BEST_HAND_SIZE], int tb[BEST_HAND_SIZE]) {
    int ranks[BEST_HAND_SIZE];
    int suits[BEST_HAND_SIZE];
    for (int i = 0; i < BEST_HAND_SIZE; i++) {
        ranks[i] = hand[i].rank;
        suits[i] = hand[i].suit;
    }

    sort_ranks_desc(ranks);

    /* Flush: all five suits identical. */
    int is_flush = 1;
    for (int i = 1; i < BEST_HAND_SIZE; i++) {
        if (suits[i] != suits[0]) { is_flush = 0; break; }
    }

    /* Straight: five consecutive distinct ranks (descending after sort). */
    int is_straight = 0;
    if (ranks[0] - ranks[BEST_HAND_SIZE - 1] == 4 &&
        ranks[0] != ranks[1] && ranks[1] != ranks[2] &&
        ranks[2] != ranks[3] && ranks[3] != ranks[4]) {
        is_straight = 1;
    }

    /* Wheel straight (A-2-3-4-5): the Ace (12) plays low. Re-map so the 5 is
     * the high card and the Ace drops below the deuce, ensuring correct
     * ranking against higher straights. */
    if (ranks[0] == 12 && ranks[1] == 3 && ranks[2] == 2 &&
        ranks[3] == 1  && ranks[4] == 0) {
        is_straight = 1;
        ranks[0] = 3; ranks[1] = 2; ranks[2] = 1; ranks[3] = 0; ranks[4] = -1;
    }

    /* Per-rank counts (skip the wheel's low-Ace sentinel of -1). */
    int counts[NUM_RANKS] = {0};
    for (int i = 0; i < BEST_HAND_SIZE; i++) {
        if (ranks[i] >= 0) counts[ranks[i]]++;
    }

    int pairs = 0, trips = 0, quads = 0;
    for (int r = 0; r < NUM_RANKS; r++) {
        if (counts[r] == 2) pairs++;
        else if (counts[r] == 3) trips++;
        else if (counts[r] == 4) quads++;
    }

    /* Categorize from strongest to weakest; first match wins. */
    int rank;
    if      (is_straight && is_flush && ranks[0] == 12) rank = HAND_ROYAL_FLUSH;
    else if (is_straight && is_flush)                   rank = HAND_STRAIGHT_FLUSH;
    else if (quads)                                     rank = HAND_FOUR_OF_KIND;
    else if (trips && pairs)                            rank = HAND_FULL_HOUSE;
    else if (is_flush)                                  rank = HAND_FLUSH;
    else if (is_straight)                               rank = HAND_STRAIGHT;
    else if (trips)                                     rank = HAND_THREE_OF_KIND;
    else if (pairs == 2)                                rank = HAND_TWO_PAIR;
    else if (pairs == 1)                                rank = HAND_ONE_PAIR;
    else                                                rank = HAND_HIGH_CARD;

    build_tiebreak(counts, tb);
    return rank;
}

/* ------------------------------------------------------------------------- */
/* Best-of-pool selection                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Select the strongest five-card hand from a wild-free pool.
 *
 * @param[in]  pool     Pool of ordinary cards.
 * @param[in]  n        Number of cards in @p pool (>= 5).
 * @param[out] best5    Receives the chosen five cards.
 * @param[out] tb       Receives the chosen hand's tiebreakers.
 * @return The best hand category found, or -1 if @p n < 5.
 */
static int best_of_pool(const Card *pool, int n,
                        Card best5[BEST_HAND_SIZE], int tb[BEST_HAND_SIZE]) {
    int best_rank = -1;
    int best_tb[BEST_HAND_SIZE] = {-1, -1, -1, -1, -1};

    for (int a = 0;     a < n - 4; a++)
    for (int b = a + 1; b < n - 3; b++)
    for (int c = b + 1; c < n - 2; c++)
    for (int d = c + 1; d < n - 1; d++)
    for (int e = d + 1; e < n;     e++) {
        const Card combo[BEST_HAND_SIZE] = {
            pool[a], pool[b], pool[c], pool[d], pool[e]
        };
        int combo_tb[BEST_HAND_SIZE];
        int combo_rank = score_five(combo, combo_tb);

        int better = (combo_rank > best_rank) ||
                     (combo_rank == best_rank && tiebreak_beats(combo_tb, best_tb));
        if (better) {
            best_rank = combo_rank;
            memcpy(best_tb, combo_tb, sizeof(best_tb));
            memcpy(best5,  combo,    sizeof(Card) * BEST_HAND_SIZE);
        }
    }

    memcpy(tb, best_tb, sizeof(int) * BEST_HAND_SIZE);
    return best_rank;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int evaluate_best_hand(const Card *pool, int pool_size,
                       Card best5_out[BEST_HAND_SIZE],
                       char hand_name_out[32]) {
    /* Separate ordinary cards from the wild and note whether a wild is present. */
    Card normals[MAX_POOL];
    int  normal_count = 0;
    int  has_wild = 0;

    for (int i = 0; i < pool_size && normal_count < MAX_POOL; i++) {
        if (pool[i].is_wild) has_wild = 1;
        else                 normals[normal_count++] = pool[i];
    }

    int  best_rank = -1;
    int  best_tb[BEST_HAND_SIZE] = {-1, -1, -1, -1, -1};
    Card best5[BEST_HAND_SIZE];

    if (!has_wild) {
        best_rank = best_of_pool(normals, normal_count, best5, best_tb);
    } else {
        /* Try every concrete card the wild could become; keep the best. */
        for (int wr = 0; wr < NUM_RANKS; wr++) {
            for (int ws = 0; ws < NUM_SUITS; ws++) {
                /* Skip a card already present (no duplicates in a single deck). */
                int duplicate = 0;
                for (int i = 0; i < normal_count; i++) {
                    if (normals[i].rank == wr && normals[i].suit == ws) {
                        duplicate = 1;
                        break;
                    }
                }
                if (duplicate) continue;

                Card pool2[MAX_POOL];
                memcpy(pool2, normals, sizeof(Card) * normal_count);
                pool2[normal_count].rank    = (int8_t)wr;
                pool2[normal_count].suit    = (int8_t)ws;
                pool2[normal_count].is_wild = 1; /* tag origin for used_wild detection */

                Card cand5[BEST_HAND_SIZE];
                int  cand_tb[BEST_HAND_SIZE];
                int  cand_rank = best_of_pool(pool2, normal_count + 1, cand5, cand_tb);

                int better = (cand_rank > best_rank) ||
                             (cand_rank == best_rank && tiebreak_beats(cand_tb, best_tb));
                if (better) {
                    best_rank = cand_rank;
                    memcpy(best_tb, cand_tb, sizeof(best_tb));
                    memcpy(best5,  cand5,   sizeof(Card) * BEST_HAND_SIZE);
                }
            }
        }
    }

    if (best5_out != NULL && best_rank >= 0) {
        memcpy(best5_out, best5, sizeof(Card) * BEST_HAND_SIZE);
    }
    if (hand_name_out != NULL) {
        snprintf(hand_name_out, 32, "%s",
                 (best_rank >= 0) ? HAND_RANK_NAMES[best_rank] : "Unknown");
    }
    return best_rank;
}

int compare_hands(int rank_a, const int tb_a[BEST_HAND_SIZE],
                  int rank_b, const int tb_b[BEST_HAND_SIZE]) {
    if (rank_a != rank_b) {
        return rank_a - rank_b;
    }
    for (int i = 0; i < BEST_HAND_SIZE; i++) {
        if (tb_a[i] != tb_b[i]) {
            return tb_a[i] - tb_b[i];
        }
    }
    return 0;
}
