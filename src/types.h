#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

/* Sizes and gameplay constants                                              */

#define MAX_PLAYERS      6      /**< Maximum seats at a table.               */
#define MAX_NAME_LEN     32     /**< Buffer size for a username (incl. NUL). */
#define MAX_MSG_LEN      512    /**< Buffer size for one protocol message.   */
#define DECK_SIZE        53     /**< 52 standard cards + 1 Anteater Wild.    */
#define COMMUNITY_SIZE   5      /**< Shared community cards on the board.    */
#define HAND_SIZE        2      /**< Private hole cards dealt per player.    */
#define DEFAULT_PORT     9000   /**< Default TCP listening port.             */
#define DEFAULT_POINTS   1000   /**< Starting chip count for each player.    */
#define WILDCARD_BONUS   200    /**< Bonus awarded for winning with the wild.*/

/* Card-domain bounds (named to avoid "magic numbers" across the codebase).  */
#define NUM_RANKS        13     /**< Distinct ranks: 2,3,...,K,A.            */
#define NUM_SUITS        4      /**< Distinct suits: C,D,H,S.                */
#define BEST_HAND_SIZE   5      /**< Cards in a scored poker hand.           */

/* Sentinel values that identify the Anteater Wild Card.                     */
#define WILD_RANK        13     /**< Out-of-range rank marking the wild.     */
#define WILD_SUIT        4      /**< Out-of-range suit marking the wild.     */

/* Core data model                                                           */


typedef struct {
    int8_t rank;     /**< 0..12 (2..A), or WILD_RANK for the wild card.      */
    int8_t suit;     /**< 0..3  (C,D,H,S), or WILD_SUIT for the wild card.   */
    int8_t is_wild;  /**< Non-zero if this is the Anteater Wild Card.        */
} Card;


typedef struct {
    char name[MAX_NAME_LEN]; /**< Player username (NUL-terminated).          */
    int  points;             /**< Remaining chips.                          */
    int  seat;               /**< Seat index 0..MAX_PLAYERS-1; -1 if unseated.*/
    int  current_bet;        /**< Chips committed this betting round.        */
    int  folded;             /**< Non-zero if the player folded this hand.   */
    int  active;             /**< Non-zero if connected and in the game.     */
    int  used_wild;          /**< Non-zero if the wild appears in the best hand.*/
    Card hand[HAND_SIZE];    /**< The player's private hole cards.           */
    Card wild_card;          /**< The dealt wild card, if any.              */
} Player;


typedef enum {
    ROUND_PREFLOP  = 0,
    ROUND_FLOP     = 1,
    ROUND_TURN     = 2,
    ROUND_RIVER    = 3,
    ROUND_SHOWDOWN = 4
} BettingRound;

/* Special seat sentinels for Table::winner_seat.                            */
#define WINNER_NONE  (-1)   /**< No winner decided yet.                      */
#define WINNER_TIE   (-2)   /**< Pot is split between tied players.          */


typedef struct {
    Player players[MAX_PLAYERS];        /**< All seats (active or not).      */
    int    num_players;                 /**< Count of seated players.        */
    Card   community[COMMUNITY_SIZE];   /**< Community cards.                */
    int    community_count;             /**< Community cards revealed so far.*/
    int    pot;                         /**< Current pot in chips.           */
    int    current_bet;                 /**< Bet each active player must match.*/
    int    dealer_seat;                 /**< Seat holding the dealer button. */
    int    current_turn;                /**< Seat whose turn it is to act.   */
    int    round;                       /**< @see BettingRound.              */
    int    game_started;                /**< Non-zero once a hand is underway.*/
    int    hand_over;                   /**< Non-zero when the hand has ended.*/
    int    winner_seat;                 /**< Seat index, WINNER_NONE or WINNER_TIE.*/
    char   winner_hand_name[32];        /**< Human-readable winning hand name.*/
} Table;

/* Network protocol                                                          */


/* Client -> Server */
#define MSG_JOIN     "JOIN"     /**< JOIN|username                          */
#define MSG_SEAT     "SEAT"     /**< SEAT|seat_number                       */
#define MSG_READY    "READY"    /**< READY                                  */
#define MSG_ACTION   "ACTION"   /**< ACTION|CALL|RAISE|FOLD|CHECK|amount     */
#define MSG_WILDUSE  "WILDUSE"  /**< WILDUSE|rank|suit                      */
#define MSG_CHAT     "CHAT"     /**< CHAT|message                           */
#define MSG_LEAVE    "LEAVE"    /**< LEAVE                                   */

/* Server -> Client */
#define MSG_WELCOME    "WELCOME"   /**< WELCOME|seat|points                 */
#define MSG_SEATS      "SEATS"     /**< SEATS|name0|...|name5               */
#define MSG_DEAL       "DEAL"      /**< DEAL|r0|s0|r1|s1|wr|ws              */
#define MSG_COMMUNITY  "COMMUNITY" /**< COMMUNITY|r0|s0|...                 */
#define MSG_TURN       "TURN"      /**< TURN|seat|current_bet|pot           */
#define MSG_POT        "POT"       /**< POT|amount                          */
#define MSG_RESULT     "RESULT"    /**< RESULT|winner_seat|hand|pot|bonus   */
#define MSG_POINTS     "POINTS"    /**< POINTS|pts0|...|pts5                */
#define MSG_SHOWCARDS  "SHOWCARDS" /**< SHOWCARDS|seat|name|r0|s0|r1|s1|wild*/
#define MSG_CHAT_BC    "CHATBC"    /**< CHATBC|from_name|message            */
#define MSG_ERROR      "ERROR"     /**< ERROR|message                       */
#define MSG_INFO       "INFO"      /**< INFO|message                        */
#define MSG_GAMESTATE  "GAMESTATE" /**< Reserved: full table state.         */

/* Display lookup tables                                                     */


extern const char *const HAND_RANK_NAMES[10]; /**< Indexed by hand rank 0..9.*/
extern const char *const RANK_STR[NUM_RANKS + 1]; /**< "2".."A", "WILD".     */
extern const char *const SUIT_STR[NUM_SUITS + 1]; /**< Suit glyphs + wild.   */

#endif /* TYPES_H */
