#include <string.h>
#include <stdio.h>
#include "eval.h"
#include "types.h"

#define HR_HIGH        0
#define HR_PAIR        1
#define HR_TWO_PAIR    2
#define HR_TRIPS       3
#define HR_STRAIGHT    4
#define HR_FLUSH       5
#define HR_FULL_HOUSE  6
#define HR_QUADS       7
#define HR_STR_FLUSH   8
#define HR_ROYAL       9

static int eval5(Card h[5], int tb[5]) {
    int ranks[5], suits[5];
    for (int i = 0; i < 5; i++) { ranks[i] = h[i].rank; suits[i] = h[i].suit; }

    for (int i = 0; i < 4; i++)
        for (int j = i+1; j < 5; j++)
            if (ranks[j] > ranks[i]) { int t=ranks[i]; ranks[i]=ranks[j]; ranks[j]=t; }

    int flush = 1;
    for (int i = 1; i < 5; i++) if (suits[i] != suits[0]) { flush = 0; break; }

//straight
    int straight = 0;
    if (ranks[0]-ranks[4]==4 && ranks[0]!=ranks[1] && ranks[1]!=ranks[2]
                              && ranks[2]!=ranks[3] && ranks[3]!=ranks[4])
        straight = 1;

    if (ranks[0]==12 && ranks[1]==3 && ranks[2]==2 && ranks[3]==1 && ranks[4]==0) {
        straight = 1;
        ranks[0]=3; ranks[1]=2; ranks[2]=1; ranks[3]=0; ranks[4]=-1;
    }

    int cnt[13] = {0};
    for (int i = 0; i < 5; i++) if (ranks[i] >= 0) cnt[ranks[i]]++;

    int pairs=0, trips=0, quads=0;
    for (int r=0;r<13;r++) {
        if (cnt[r]==2) pairs++;
        if (cnt[r]==3) trips++;
        if (cnt[r]==4) quads++;
    }

    int hr;
    if      (straight && flush && ranks[0]==12) hr = HR_ROYAL;
    else if (straight && flush)                  hr = HR_STR_FLUSH;
    else if (quads)                              hr = HR_QUADS;
    else if (trips && pairs)                     hr = HR_FULL_HOUSE;
    else if (flush)                              hr = HR_FLUSH;
    else if (straight)                           hr = HR_STRAIGHT;
    else if (trips)                              hr = HR_TRIPS;
    else if (pairs == 2)                         hr = HR_TWO_PAIR;
    else if (pairs == 1)                         hr = HR_PAIR;
    else                                         hr = HR_HIGH;

//ranks
    int out[5]; int oi = 0;
    for (int freq = 4; freq >= 1; freq--)
        for (int r = 12; r >= 0; r--)
            if (cnt[r] == freq)
                for (int k = 0; k < freq && oi < 5; k++)
                    out[oi++] = r;

    while (oi < 5) out[oi++] = -1;
    for (int i = 0; i < 5; i++) tb[i] = out[i];

    return hr;
}

static int best_from_pool_nowild(Card *pool, int n, Card best5[5], int tb[5]) {
    int best_hr = -1;
    int best_tb[5] = {-1,-1,-1,-1,-1};

    for (int a=0;a<n-4;a++)
    for (int b=a+1;b<n-3;b++)
    for (int c=b+1;c<n-2;c++)
    for (int d=c+1;d<n-1;d++)
    for (int e=d+1;e<n;e++) {
        Card h[5] = {pool[a],pool[b],pool[c],pool[d],pool[e]};
        int ltb[5];
        int hr = eval5(h, ltb);
        int better = (hr > best_hr);
        if (!better && hr == best_hr)
            for (int i=0;i<5;i++) {
                if (ltb[i] > best_tb[i]) { better=1; break; }
                if (ltb[i] < best_tb[i]) break;
            }
        if (better) {
            best_hr = hr;
            memcpy(best_tb, ltb, sizeof(ltb));
            memcpy(best5, h, sizeof(Card)*5);
        }
    }
    memcpy(tb, best_tb, sizeof(int)*5);
    return best_hr;
}

int evaluate_best_hand(Card *pool, int pool_size,
                       Card best5_out[5], char hand_name_out[32]) {

    Card normals[16]; int nn = 0;
    int has_wild = 0;
    for (int i = 0; i < pool_size; i++) {
        if (pool[i].is_wild) has_wild = 1;
        else normals[nn++] = pool[i];
    }

    int best_hr = -1;
    int best_tb[5] = {-1,-1,-1,-1,-1};
    Card best5[5];

    if (!has_wild) {
        best_hr = best_from_pool_nowild(normals, nn, best5, best_tb);
    } else {

        for (int wr = 0; wr < 13; wr++)
        for (int ws = 0; ws < 4;  ws++) {

            int dup = 0;
            for (int i = 0; i < nn; i++)
                if (normals[i].rank==wr && normals[i].suit==ws) { dup=1; break; }
            if (dup) continue;

            Card pool2[16]; memcpy(pool2, normals, sizeof(Card)*nn);
            pool2[nn].rank = wr; pool2[nn].suit = ws; pool2[nn].is_wild = 0;
            int n2 = nn + 1;

            Card h5[5]; int ltb[5];
            int hr = best_from_pool_nowild(pool2, n2, h5, ltb);

            int better = (hr > best_hr);
            if (!better && hr == best_hr)
                for (int i=0;i<5;i++) {
                    if (ltb[i] > best_tb[i]) { better=1; break; }
                    if (ltb[i] < best_tb[i]) break;
                }
            if (better) {
                best_hr = hr;
                memcpy(best_tb, ltb, sizeof(ltb));
                memcpy(best5, h5, sizeof(Card)*5);
            }
        }
    }

    memcpy(best5_out, best5, sizeof(Card)*5);
    snprintf(hand_name_out, 32, "%s",
             best_hr >= 0 ? HAND_RANK_NAMES[best_hr] : "Unknown");
    return best_hr;
}

int compare_hands(int rank_a, int tb_a[5], int rank_b, int tb_b[5]) {
    if (rank_a != rank_b) return rank_a - rank_b;
    for (int i = 0; i < 5; i++)
        if (tb_a[i] != tb_b[i]) return tb_a[i] - tb_b[i];
    return 0;
}
