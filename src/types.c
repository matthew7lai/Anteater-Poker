#include "types.h"

/** Human-readable hand names, indexed by hand rank (0 = High Card .. 9 = Royal). */
const char *const HAND_RANK_NAMES[10] = {
    "High Card", "One Pair", "Two Pair", "Three of a Kind",
    "Straight", "Flush", "Full House", "Four of a Kind",
    "Straight Flush", "Royal Flush"
};

/** Rank glyphs, indexed 0..12 (2..A); index NUM_RANKS is the wild marker. */
const char *const RANK_STR[NUM_RANKS + 1] = {
    "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A", "WILD"
};

/** Suit glyphs, indexed 0..3 (C,D,H,S); index NUM_SUITS is the wild marker. */
const char *const SUIT_STR[NUM_SUITS + 1] = {
    "\u2663", "\u2666", "\u2665", "\u2660", "*"  /* clubs diamonds hearts spades */
};
