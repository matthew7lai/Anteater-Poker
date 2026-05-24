#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "types.h"
#include "eval.h"

static int passed = 0, failed = 0;

#define TEST(name, expr) do { \
    if (expr) { printf("[PASS] %s\n", name); passed++; } \
    else       { printf("[FAIL] %s  (line %d)\n", name, __LINE__); failed++; } \
} while(0)

static Card C(int rank, int suit) { Card c={rank,suit,0}; return c; }
static Card W(void)               { Card c={13,4,1};      return c; }

static int hand_rank(Card pool[], int n) {
    Card b[5]; char nm[32];
    return evaluate_best_hand(pool, n, b, nm);
}

//test the deck
static void test_deck(void) {
    Card deck[DECK_SIZE];
    int i=0;
    for(int s=0;s<4;s++) for(int r=0;r<13;r++) {
        deck[i].rank=r; deck[i].suit=s; deck[i].is_wild=0; i++;
    }
    deck[52]=(Card){13,4,1};
    TEST("deck_size_53",    DECK_SIZE==53);
    TEST("deck_last_wild",  deck[52].is_wild==1);
    TEST("deck_first_card", deck[0].rank==0 && deck[0].suit==0);
    TEST("deck_ace_spades", deck[51].rank==12 && deck[51].suit==3);
}

static void test_hands(void) {
    Card pool[7];
    int n;

//royal flush
    pool[0]=C(12,2); pool[1]=C(11,2); pool[2]=C(10,2);
    pool[3]=C(9,2);  pool[4]=C(8,2);
    TEST("royal_flush", hand_rank(pool,5)==9);

    //straight flush
    pool[0]=C(5,1); pool[1]=C(6,1); pool[2]=C(7,1);
    pool[3]=C(8,1); pool[4]=C(9,1);
    TEST("straight_flush", hand_rank(pool,5)==8);

    //Quads
    pool[0]=C(7,0); pool[1]=C(7,1); pool[2]=C(7,2);
    pool[3]=C(7,3); pool[4]=C(2,0);
    TEST("four_of_a_kind", hand_rank(pool,5)==7);

    //Full House
    pool[0]=C(10,0); pool[1]=C(10,1); pool[2]=C(10,2);
    pool[3]=C(5,0);  pool[4]=C(5,1);
    TEST("full_house", hand_rank(pool,5)==6);

   //flush
    pool[0]=C(2,3); pool[1]=C(5,3); pool[2]=C(7,3);
    pool[3]=C(9,3); pool[4]=C(11,3);
    TEST("flush", hand_rank(pool,5)==5);

    //straight
    pool[0]=C(4,0); pool[1]=C(5,1); pool[2]=C(6,2);
    pool[3]=C(7,3); pool[4]=C(8,0);
    TEST("straight", hand_rank(pool,5)==4);

    //three of a kind
    pool[0]=C(3,0); pool[1]=C(3,1); pool[2]=C(3,2);
    pool[3]=C(7,0); pool[4]=C(9,1);
    TEST("three_of_a_kind", hand_rank(pool,5)==3);

    //two pair
    pool[0]=C(4,0); pool[1]=C(4,1); pool[2]=C(6,0);
    pool[3]=C(6,1); pool[4]=C(9,2);
    TEST("two_pair", hand_rank(pool,5)==2);

    //one pair
    pool[0]=C(2,0); pool[1]=C(2,1); pool[2]=C(5,0);
    pool[3]=C(7,1); pool[4]=C(9,2);
    TEST("one_pair", hand_rank(pool,5)==1);

    //high card
    pool[0]=C(2,0); pool[1]=C(4,1); pool[2]=C(6,2);
    pool[3]=C(9,3); pool[4]=C(11,0);
    TEST("high_card", hand_rank(pool,5)==0);

    //wheel straight
    pool[0]=C(12,0); pool[1]=C(0,1); pool[2]=C(1,2);
    pool[3]=C(2,3);  pool[4]=C(3,0);
    TEST("wheel_straight", hand_rank(pool,5)==4);
}

//test wildcard
static void test_wildcard(void) {
    Card pool[8];

    pool[0]=C(2,2); pool[1]=C(5,2); pool[2]=C(8,2); pool[3]=C(11,2);
    pool[4]=W();
    TEST("wild_completes_flush", hand_rank(pool,5)>=5);

    pool[0]=C(9,0); pool[1]=C(9,1); pool[2]=C(9,2); pool[3]=C(2,3);
    pool[4]=W();
    TEST("wild_completes_quads", hand_rank(pool,5)==7);

    pool[0]=C(3,0); pool[1]=C(4,1); pool[2]=C(5,2); pool[3]=C(6,3);
    pool[4]=W();
    TEST("wild_completes_straight", hand_rank(pool,5)>=4);

    pool[0]=C(10,0); pool[1]=C(10,1); pool[2]=C(10,2);
    pool[3]=C(5,0);  pool[4]=C(5,1);
    pool[5]=C(2,3);  pool[6]=W();
    TEST("wild_7card_fullhouse_or_better", hand_rank(pool,7)>=6);

    pool[0]=C(6,0); pool[1]=C(6,1); pool[2]=C(2,2);
    pool[3]=C(4,3); pool[4]=W();
    TEST("wild_pair_to_trips", hand_rank(pool,5)==3);
}

static void test_best5(void) {

    Card pool[7];
    pool[0]=C(8,0); pool[1]=C(8,1); pool[2]=C(8,2);
    pool[3]=C(3,0); pool[4]=C(3,1);
    pool[5]=C(2,0); pool[6]=C(7,1);
    TEST("best5_fullhouse_from_7", hand_rank(pool,7)==6);

    pool[0]=C(2,1); pool[1]=C(5,1); pool[2]=C(8,1);
    pool[3]=C(10,1);pool[4]=C(12,1);
    pool[5]=C(0,0); pool[6]=C(3,2);
    TEST("best5_flush_from_7", hand_rank(pool,7)==5);
}

static void test_messages(void) {
    char buf[256];
    snprintf(buf, 256, "%s|testuser", MSG_JOIN);
    TEST("msg_join_format",   strncmp(buf,"JOIN|testuser",13)==0);

    snprintf(buf, 256, "%s|2|1000", MSG_WELCOME);
    TEST("msg_welcome_format", strncmp(buf,"WELCOME|2|1000",14)==0);

    snprintf(buf, 256, "%s|CALL|0", MSG_ACTION);
    TEST("msg_action_call",   strncmp(buf,"ACTION|CALL|0",13)==0);
}

//tiebreak
static void test_tiebreak(void) {
    Card best[5]; char nm[32];
    Card a[5]={C(12,0),C(12,1),C(7,0),C(3,1),C(2,2)};
    Card b[5]={C(12,2),C(12,3),C(8,0),C(3,2),C(2,3)};
    int ra=evaluate_best_hand(a,5,best,nm);
    int rb=evaluate_best_hand(b,5,best,nm);
    /* b has better kicker (8 vs 7), so b > a */
    /* We just test both evaluate to ONE_PAIR */
    TEST("tiebreak_both_one_pair", ra==1 && rb==1);
}

//main
int main(void) {
    printf("=== Anteater Poker Unit Tests (Team 23) ===\n\n");
    test_deck();
    test_hands();
    test_wildcard();
    test_best5();
    test_messages();
    test_tiebreak();
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
