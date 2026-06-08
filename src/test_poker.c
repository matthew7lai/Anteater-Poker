#include <stdio.h>
#include <string.h>

#include "types.h"
#include "eval.h"

/* Minimal test harness                                                      */

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name, expr)                                             \
    do {                                                            \
        if (expr) {                                                 \
            printf("[PASS] %s\n", (name));                          \
            g_passed++;                                             \
        } else {                                                    \
            printf("[FAIL] %s  (line %d)\n", (name), __LINE__);     \
            g_failed++;                                             \
        }                                                           \
    } while (0)

/* Card construction helpers                                                 */

/** Build an ordinary card. */
static Card card(int rank, int suit) {
    Card c = { (int8_t)rank, (int8_t)suit, 0 };
    return c;
}

/** Build an Anteater Wild Card. */
static Card wild(void) {
    Card c = { WILD_RANK, WILD_SUIT, 1 };
    return c;
}

/** Convenience: score a pool and return only the hand category. */
static int rank_of(const Card *pool, int n) {
    Card best[BEST_HAND_SIZE];
    char name[32];
    return evaluate_best_hand(pool, n, best, name);
}

/* Deck construction                                                         */

static void test_deck(void) {
    Card deck[DECK_SIZE];
    int  i = 0;
    for (int s = 0; s < NUM_SUITS; s++) {
        for (int r = 0; r < NUM_RANKS; r++) {
            deck[i].rank = (int8_t)r;
            deck[i].suit = (int8_t)s;
            deck[i].is_wild = 0;
            i++;
        }
    }
    deck[52] = wild();

    TEST("deck_size_53",    DECK_SIZE == 53);
    TEST("deck_last_wild",  deck[52].is_wild == 1);
    TEST("deck_first_card", deck[0].rank == 0 && deck[0].suit == 0);
    TEST("deck_ace_spades", deck[51].rank == 12 && deck[51].suit == 3);
}

/* Hand categories                                                           */

static void test_hands(void) {
    Card pool[7];

    pool[0]=card(12,2); pool[1]=card(11,2); pool[2]=card(10,2);
    pool[3]=card(9,2);  pool[4]=card(8,2);
    TEST("royal_flush", rank_of(pool, 5) == HAND_ROYAL_FLUSH);

    pool[0]=card(5,1); pool[1]=card(6,1); pool[2]=card(7,1);
    pool[3]=card(8,1); pool[4]=card(9,1);
    TEST("straight_flush", rank_of(pool, 5) == HAND_STRAIGHT_FLUSH);

    pool[0]=card(7,0); pool[1]=card(7,1); pool[2]=card(7,2);
    pool[3]=card(7,3); pool[4]=card(2,0);
    TEST("four_of_a_kind", rank_of(pool, 5) == HAND_FOUR_OF_KIND);

    pool[0]=card(10,0); pool[1]=card(10,1); pool[2]=card(10,2);
    pool[3]=card(5,0);  pool[4]=card(5,1);
    TEST("full_house", rank_of(pool, 5) == HAND_FULL_HOUSE);

    pool[0]=card(2,3); pool[1]=card(5,3); pool[2]=card(7,3);
    pool[3]=card(9,3); pool[4]=card(11,3);
    TEST("flush", rank_of(pool, 5) == HAND_FLUSH);

    pool[0]=card(4,0); pool[1]=card(5,1); pool[2]=card(6,2);
    pool[3]=card(7,3); pool[4]=card(8,0);
    TEST("straight", rank_of(pool, 5) == HAND_STRAIGHT);

    pool[0]=card(3,0); pool[1]=card(3,1); pool[2]=card(3,2);
    pool[3]=card(7,0); pool[4]=card(9,1);
    TEST("three_of_a_kind", rank_of(pool, 5) == HAND_THREE_OF_KIND);

    pool[0]=card(4,0); pool[1]=card(4,1); pool[2]=card(6,0);
    pool[3]=card(6,1); pool[4]=card(9,2);
    TEST("two_pair", rank_of(pool, 5) == HAND_TWO_PAIR);

    pool[0]=card(2,0); pool[1]=card(2,1); pool[2]=card(5,0);
    pool[3]=card(7,1); pool[4]=card(9,2);
    TEST("one_pair", rank_of(pool, 5) == HAND_ONE_PAIR);

    pool[0]=card(2,0); pool[1]=card(4,1); pool[2]=card(6,2);
    pool[3]=card(9,3); pool[4]=card(11,0);
    TEST("high_card", rank_of(pool, 5) == HAND_HIGH_CARD);

    pool[0]=card(12,0); pool[1]=card(0,1); pool[2]=card(1,2);
    pool[3]=card(2,3);  pool[4]=card(3,0);
    TEST("wheel_straight", rank_of(pool, 5) == HAND_STRAIGHT);
}

/* Wild card resolution                                                      */

static void test_wildcard(void) {
    Card pool[8];

    pool[0]=card(2,2); pool[1]=card(5,2); pool[2]=card(8,2); pool[3]=card(11,2);
    pool[4]=wild();
    TEST("wild_completes_flush", rank_of(pool, 5) >= HAND_FLUSH);

    pool[0]=card(9,0); pool[1]=card(9,1); pool[2]=card(9,2); pool[3]=card(2,3);
    pool[4]=wild();
    TEST("wild_completes_quads", rank_of(pool, 5) == HAND_FOUR_OF_KIND);

    pool[0]=card(3,0); pool[1]=card(4,1); pool[2]=card(5,2); pool[3]=card(6,3);
    pool[4]=wild();
    TEST("wild_completes_straight", rank_of(pool, 5) >= HAND_STRAIGHT);

    pool[0]=card(10,0); pool[1]=card(10,1); pool[2]=card(10,2);
    pool[3]=card(5,0);  pool[4]=card(5,1);
    pool[5]=card(2,3);  pool[6]=wild();
    TEST("wild_7card_fullhouse_or_better", rank_of(pool, 7) >= HAND_FULL_HOUSE);

    pool[0]=card(6,0); pool[1]=card(6,1); pool[2]=card(2,2);
    pool[3]=card(4,3); pool[4]=wild();
    TEST("wild_pair_to_trips", rank_of(pool, 5) == HAND_THREE_OF_KIND);

    /* The resolved wild must be detectable in the chosen hand so the server
     * can award the wild bonus. */
    Card best[BEST_HAND_SIZE];
    char name[32];
    pool[0]=card(2,2); pool[1]=card(5,2); pool[2]=card(8,2); pool[3]=card(11,2);
    pool[4]=wild();
    evaluate_best_hand(pool, 5, best, name);
    int wild_seen = 0;
    for (int k = 0; k < BEST_HAND_SIZE; k++) {
        if (best[k].is_wild) wild_seen = 1;
    }
    TEST("wild_flag_propagates_to_best_hand", wild_seen == 1);
}

/* Best-five-of-seven selection                                              */

static void test_best5(void) {
    Card pool[7];

    pool[0]=card(8,0); pool[1]=card(8,1); pool[2]=card(8,2);
    pool[3]=card(3,0); pool[4]=card(3,1);
    pool[5]=card(2,0); pool[6]=card(7,1);
    TEST("best5_fullhouse_from_7", rank_of(pool, 7) == HAND_FULL_HOUSE);

    pool[0]=card(2,1); pool[1]=card(5,1); pool[2]=card(8,1);
    pool[3]=card(10,1);pool[4]=card(12,1);
    pool[5]=card(0,0); pool[6]=card(3,2);
    TEST("best5_flush_from_7", rank_of(pool, 7) == HAND_FLUSH);
}

/* Comparison and tiebreaking                                                */

static void test_compare(void) {
    Card best[BEST_HAND_SIZE];
    char name[32];

    /* Both one pair (pair of aces) with different kickers. */
    Card a[5] = { card(12,0), card(12,1), card(7,0), card(3,1), card(2,2) };
    Card b[5] = { card(12,2), card(12,3), card(8,0), card(3,2), card(2,3) };
    int ra = evaluate_best_hand(a, 5, best, name);
    int rb = evaluate_best_hand(b, 5, best, name);
    TEST("compare_both_one_pair", ra == HAND_ONE_PAIR && rb == HAND_ONE_PAIR);

    /* compare_hands convention: >0 A wins, <0 B wins, 0 tie. */
    int tb_hi[BEST_HAND_SIZE] = {12, 12, 8, 3, 2};
    int tb_lo[BEST_HAND_SIZE] = {12, 12, 7, 3, 2};
    TEST("compare_kicker_breaks_tie",
         compare_hands(HAND_ONE_PAIR, tb_hi, HAND_ONE_PAIR, tb_lo) > 0);
    TEST("compare_exact_tie",
         compare_hands(HAND_FLUSH, tb_hi, HAND_FLUSH, tb_hi) == 0);
    TEST("compare_category_dominates",
         compare_hands(HAND_FLUSH, tb_lo, HAND_STRAIGHT, tb_hi) > 0);
}

/* Protocol message formatting                                               */

static void test_messages(void) {
    char buf[256];

    snprintf(buf, sizeof(buf), "%s|testuser", MSG_JOIN);
    TEST("msg_join_format", strcmp(buf, "JOIN|testuser") == 0);

    snprintf(buf, sizeof(buf), "%s|2|1000", MSG_WELCOME);
    TEST("msg_welcome_format", strcmp(buf, "WELCOME|2|1000") == 0);

    snprintf(buf, sizeof(buf), "%s|CALL|0", MSG_ACTION);
    TEST("msg_action_call", strcmp(buf, "ACTION|CALL|0") == 0);
}

/* Entry point                                                               */

int main(void) {
    printf("=== Anteater Poker Unit Tests (Team 23) ===\n\n");

    test_deck();
    test_hands();
    test_wildcard();
    test_best5();
    test_compare();
    test_messages();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed > 0) ? 1 : 0;
}
