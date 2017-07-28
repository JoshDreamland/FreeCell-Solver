#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef USE_CURSES
constexpr bool kUseCurses = true;
#include <ncurses.h>
#else
constexpr bool kUseCurses = false;
#endif

#define DEBUG_MODE

using std::cerr;
using std::cout;
using std::endl;
using std::priority_queue;
using std::string;
using std::vector;

template<typename... K>
using u_set = std::unordered_set<K...>;

typedef uint8_t card_count_t; ///< Smallest integer that can count all cards.
constexpr card_count_t RESERVE_SIZE = 4; ///< Number of reserve slots.
constexpr card_count_t CASCADE_COUNT = 8; ///< Number of cascades (places to put a tableau).
constexpr card_count_t NUM_DECKS = 1; ///< Number of complete decks of cards.
constexpr card_count_t TOTAL_CARDS = 52 * NUM_DECKS; ///< Total number of cards in the game.

constexpr size_t GC_UPPER_BOUND = 1 << 20; ///< Maximum search space.

// Heuristic weights
constexpr size_t HEURISTIC_GREED = 32;
constexpr size_t MOVE_PUNISHMENT = 32;
constexpr size_t INACCESSIBILITY_PUNISHMENT = 32;
constexpr size_t TABLEAU_REWARD = 1;

// HEURISTIC_GREED
//   Heuristic bias toward number of cards in the foundation.
//   Smaller values consume more resource, but produce better solutions.
//
// MOVE_PUNISHMENT
//   Heuristic deduction for each move taken. Higher values produce shorter
//   solutions, at the cost of widening the search space. Do not exceed the
//   HEURISTIC_GREED parameter, or the search will fight itself. Do not go
//   below zero, or the search will simply optimize for long solutions.
//
// INACCESSIBILITY_PUNISHMENT
//   Heuristic cost of having high-value cards stacked on top of low-value
//   cards. Higher values will make the play more human-like, for better or
//   for worse. There isn't a strong correlation between this value and the
//   quality of your results—such is human play.
//
// TABLEAU_REWARD
//   Complementary to INACCESSIBILITY_PUNISHMENT, this value rewards having
//   larger stacks of decreasing card ranks. As this value approaches the other
//   heuristics, the search begins to screw around instead of solve the problem.

#define NONNULL(ref) non_null(ref, #ref, __func__, __FILE__, __LINE__)
#define NON_NULL(ref, entity) non_null(ref, entity, __func__, __FILE__, __LINE__)
template<typename T> T* non_null(T* ref, const char* entity,
    const char* funcname, const char* filename, size_t lineno) {
  if (!ref) {
    cerr << "Internal error: null " << entity << " ("
         << funcname << "@" << filename << ":" << lineno << "). Abort." << endl;
    abort();
  }
  return ref;
}

// =============================================================================
// === Representation ==========================================================
// =============================================================================

struct Card {
  union {
    struct {
      uint8_t suit : 2;
      uint8_t face : 5;
    };
    int8_t value;
  };
  
  enum Suit: uint8_t {
    SPADE, HEART, DIAMOND, CLUB
  };
  enum Face: uint8_t {
    EMPTY, A, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN, J, Q, K
  };
  
  string str() const {
    static string names[] = {
      "ERR", "Ace", "Two", "Three", "Four", "Five", "Six", "Seven",
      "Eight", "Nine", "Ten", "Jack", "Queen", "King"
    };
    static string suits[] = { "Spades", "Hearts", "Diamonds", "Clubs" };
    if (!face) return "Empty";
    return names[face] + " of " + suits[suit];
  }
  
  string chr() const {
    if (!face) return "🂠";
    int ordinal = 0x1F0A0 + 0x10 * suit + face + (face > Card::Face::J);
    string res(4, 0);
    res[0] = 0b11110000;
    res[1] = 0b10000000 | ((ordinal & (0b111111 << 12)) >> 12);
    res[2] = 0b10000000 | ((ordinal & (0b111111 << 6))  >> 6);
    res[3] = 0b10000000 | (ordinal & 0b111111);
    return res;
  }
  
  string desc() const {
    return face ? string(1, "XA23456789TJQK"[face]) + "SHDC"[suit] : "XX";
  }
  
  static bool color(Card::Suit s) {
    return ((int) s & 1) ^ (((int) s & 2) >> 1);
  }

  bool color() const {
    return color((Suit) suit);
  }
  
  operator bool() const {
    return value;
  }
  
  void clear() { value = 0; }
  
  Card(): value(0) {}
  Card(Face f, Suit s): suit(s), face(f) {}
  Card(int8_t v): value(v) {}
  Card(string desc) {
    size_t i = 0;
    while (i < desc.length() && std::isspace(desc[i])) ++i;
    const size_t f = i;
    
    if (i >= desc.length()) {
      cerr << "Failed to read card face: card descriptor is empty" << endl;
      abort();
    }
    
    while (i < desc.length() && desc[i] >= '0' && desc[i] <= '9') ++i;
    if (i == f) {
      switch (desc[i]) {
        case 'a': case 'A': face = Face::A; break;
        case 't': case 'T': face = Face::TEN; break;
        case 'j': case 'J': face = Face::J; break;
        case 'q': case 'Q': face = Face::Q; break;
        case 'k': case 'K': face = Face::K; break;
        default:
          cerr << "Failed to read card face: unknown card " << desc << endl;
          abort();
      }
      ++i;
    } else {
      int facev = atoi(desc.substr(f, i - f).c_str());
      if (facev < Face::A || facev > Face::K) {
        cerr << "Failed to read card face: card " << desc
             << " has invalid face value '" << facev << "'." << endl;
        abort();
      }
      face = (Face) facev;
    }
    
    if (i >= desc.length()) {
      cerr << "Failed to read card face: card suit is missing; got only '"
           << desc << "'" << endl;
      abort();
    }

    switch (desc[i]) {
      case 'h': case 'H': suit = Suit::HEART;   break;
      case 'd': case 'D': suit = Suit::DIAMOND; break;
      case 'c': case 'C': suit = Suit::CLUB;    break;
      case 's': case 'S': suit = Suit::SPADE;   break;
      default:
        cerr << "Failed to read card suit: unknown card '"
             << desc << "': '" << desc[i] << "' is not a suit" << endl;
        abort();
    }
    
    while (++i < desc.length()) if (!std::isspace(desc[i])) {
      cerr << "Failed to read card suit: Junk at end of card descriptor '"
           << desc << "'" << endl;
      abort();
    }
  }
};

static_assert(sizeof(Card) == 1);
typedef vector<Card> FluffyCascade;
struct FluffyBoard {
  Card reserve[RESERVE_SIZE] {};
  Card foundation[4] {};
  vector<FluffyCascade> cascades;
  
  string desc() const {
    string res;
    bool more = true;
    for (size_t i = 0; more; ++i) {
      string line = ":";
      more = false;
      for (const FluffyCascade &c : cascades) {
        if (i >= c.size()) {
          line += "   ";
          continue;
        }
        line += " " + c[i].desc();
        more = true;
      }
      if (more) res += line + "\n";
    }
    return res;
  }
  
  string str() const {
    string res;
    for (size_t i = 0; i < RESERVE_SIZE; ++i) {
      res += reserve[i].chr() + " ";
    }
    res += "       ";
    for (size_t i = 0; i < 4; ++i) {
      res += foundation[i].chr() + " ";
    }
    res += "\n\n";
    
    bool more = true;
    for (size_t i = 0; more; ++i) {
      string line;
      more = false;
      for (const FluffyCascade &c : cascades) {
        if (i >= c.size()) {
          line += line.empty() ? " " : "   ";
          continue;
        }
        if (line.length()) line += "  ";
        line += c[i].chr();
        more = true;
      }
      if (more) res += line + "\n";
    }
    return res;
  }
  
  FluffyBoard() {}
  FluffyBoard(string desc): reserve{}, foundation{} {
    size_t cascade = 0;
    std::map<int8_t, int> read_cards;
    for (size_t i = 0; i < desc.length(); ) {
      while (i < desc.length() && (desc[i] == ' ' || desc[i] == '\t')) ++i;
      if (i >= desc.length()) break;
      
      if (desc[i] == ':' || desc[i] == '\r' || desc[i] == '\n') {
        cascade = 0; ++i;
        continue;
      }
      
      const size_t f = i;
      while (i < desc.length() && !std::isspace(desc[i]) && desc[i] != ':') ++i;
      string card_desc = desc.substr(f, i - f);
      if (cascade >= cascades.size()) cascades.push_back({});
      
      Card card(card_desc);
      cascades[cascade++].push_back(card);
      ++read_cards[card.value];
    }
    if (read_cards.size() != 52) {
      cerr << "WARNING: input does not contain all 52 card faces." << endl;
      for (int s = 0; s < 4; ++s) {
        for (int f = Card::Face::A; f <= Card::Face::K; ++f) {
          Card card((Card::Face) f, (Card::Suit) s);
          if (read_cards.find(card.value) == read_cards.end()) {
            cerr << "- Missing " << card.str() << endl;
          }
        }
      }
    }
    std::map<int, int> count_counts;
    for (auto cn : read_cards) ++count_counts[cn.second];
    if (count_counts.size() > 1) {
      cerr << "WARNING: Some cards appear more frequently than others." << endl;
      for (auto &ccr : count_counts) {
        if (ccr.second < 26) {
          cerr << "- The following cards appear " << ccr.first << " times:\n";
          for (auto &cn : read_cards) {
            if (cn.second == ccr.first) {
              cerr << "  > " << Card(cn.first).str() << endl;
            }
          }
        }
      }
    }
  }
};

struct Board {
  constexpr static card_count_t CARD_BANK_SIZE = TOTAL_CARDS + CASCADE_COUNT;

  Card reserve[RESERVE_SIZE];
  Card cards[CARD_BANK_SIZE];
  card_count_t foundation[4] {};
  card_count_t cascade_divs[CASCADE_COUNT]; // Index after each cascade.
  
  struct CascadeView {
    const Card *const card;
    const card_count_t size;
    
    bool empty() const {
      return !size;
    }
    
    Card back() const {
      return card[size - 1];
    }
  };
  
  CascadeView cascade(card_count_t i) const {
    card_count_t base = i ? cascade_divs[i - 1] + 1 : 0;
    card_count_t length = cascade_divs[i] - base;
    return { cards + base, length };
  }
  
  /// Returns the last card in the cascade, or the 0 card for the empty cascade.
  Card cascade_back(card_count_t i) const {
    card_count_t ind = cascade_divs[i];
    return ind ? cards[ind - 1] : Card();
  }
  
  bool cascade_empty(card_count_t i) const {
    return (!i && !cascade_divs[0]) || cascade_divs[i] == 1 + cascade_divs[i-1];
  }
  
  bool reserve_full() const {
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) {
      if (!reserve[i]) return false;
    }
    return true;
  }
  
  /// Returns the index *after* the given cascade. The card will have value 0.
  card_count_t cascade_end(card_count_t i) const {
    return cascade_divs[i];
  }
  
  void copy_reserve_and_foundation(const Board &b) {
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) {
      reserve[i] = b.reserve[i];
    }
    for (card_count_t i = 0; i < 4; ++i) {
      foundation[i] = b.foundation[i];
    }
  }
  
  void copy_from_and_append(const Board &b, card_count_t cascade, Card append) {
    copy_reserve_and_foundation(b);
    
    card_count_t d = 0, i = 0;
    const card_count_t ce = b.cascade_end(cascade);
    while (i < ce) cards[d++] = b.cards[i++];
    cards[d++] = append;
    while (d < CARD_BANK_SIZE) cards[d++] = b.cards[i++];
#   ifdef DEBUG_MODE
    if (i != CARD_BANK_SIZE - 1) {
      cerr << "Logic error: copied all but one card, but buffer was read to "
           << (int) i << " / " << (int) CARD_BANK_SIZE << " cards..." << endl;
      abort();
    }
#   endif
    
    for (d = 0; d < cascade; ++d) cascade_divs[d] = b.cascade_divs[d];
    for (; d < CASCADE_COUNT; ++d) cascade_divs[d] = b.cascade_divs[d] + 1;
  }
  
  void copy_from_but_move(const Board &b,
      card_count_t from, card_count_t to, card_count_t count) {
    copy_reserve_and_foundation(b);
    
    card_count_t d = 0, s = 0;
    const card_count_t appto = b.cascade_end(to);
    const card_count_t dto = b.cascade_end(from);
    const card_count_t dfrom = dto - count;
    if (dfrom < appto) {
      while (s < dfrom) cards[d++] = b.cards[s++];
      s = dto;
      while (s < appto) cards[d++] = b.cards[s++];
      for (card_count_t cf = dfrom; cf < dto; cards[d++] = b.cards[cf++]);
      while (s < CARD_BANK_SIZE) cards[d++] = b.cards[s++];
    } else {
      while (s < appto) cards[d++] = b.cards[s++];
      for (card_count_t cf = dfrom; cf < dto; cards[d++] = b.cards[cf++]);
      while (s < dfrom) cards[d++] = b.cards[s++];
      s = dto;
      while (s < CARD_BANK_SIZE) cards[d++] = b.cards[s++];
    }
#   ifdef DEBUG_MODE
    if (s != d) {
      cerr << "Logic error: copy with internal move resulted in different card "
           << "count (suddenly " << d << " instead of " << s << ")" << endl;
      abort();
    }
#   endif
    
    // Dear optimizing compiler: please unroll this loop and pre-evaluate those
    // if statements. Thank you very much.
    for (d = 0; d < CASCADE_COUNT; ++d) {
      cascade_divs[d] = b.cascade_divs[d];
      if (d >= from) cascade_divs[d] -= count;
      if (d >= to) cascade_divs[d] += count;
    }
    // Sometimes I think the compiler ignores my comments...
  }
  
  void copy_from_but_remove(const Board &b, card_count_t cascade) {
    copy_reserve_and_foundation(b);
    
    card_count_t d = 0, i = 0;
    const card_count_t lc = b.cascade_end(cascade) - 1;
    while (i < lc) cards[d++] = b.cards[i++];
    while (++i < CARD_BANK_SIZE) cards[d++] = b.cards[i];
#   ifdef DEBUG_MODE
    if (d != CARD_BANK_SIZE - 1) {
      cerr << "Logic error: copied all but one card, but buffer is full to "
           << (int) d << " / " << (int) CARD_BANK_SIZE << " cards..." << endl;
      abort();
    }
#   endif
    cards[d].clear();
    
    for (d = 0; d < cascade; ++d) cascade_divs[d] = b.cascade_divs[d];
    for (; d < CASCADE_COUNT; ++d) cascade_divs[d] = b.cascade_divs[d] - 1;
  }
  
  void board_dump() {
    cout << "R";
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) {
      cout << " " << reserve[i].chr();
    }
    cout << "   F";
    for (card_count_t i = 0; i < 4; ++i) {
      cout << " " << Card((Card::Face) foundation[i], (Card::Suit) i).chr();
    }
    cout << endl << ":";
    for (Card c : cards) {
      if (c) cout << " " << c.chr();
      else cout << endl << ":";
    }
    cout << " END" << endl;
    abort();
  }
  
  void check_sanity() {
    int lend = -1;
    for (card_count_t i = 0; i < CASCADE_COUNT; ++i) {
      if (cascade_divs[i] >= CARD_BANK_SIZE) {
        cerr << "Junk cascade div created: cascades[" << (int) i
             << "] = " << (int) cascade_divs[i] << " ≥ " << (int) CARD_BANK_SIZE
             << endl;
        board_dump();
      }
      if (cards[cascade_divs[i]]) {
        cerr << "Junk cascade div created: cards[cascades[" << (int) i
             << "] = " << (int) cascade_divs[i]
             << "] = " << cards[cascade_divs[i]].desc() << endl;
        board_dump();
      }
      if ((int) cascade_divs[i] <= lend) {
        cerr << "Cascade " << i << " ends on or before end of previous cascade:"
             << " ends at " << (int) cascade_divs[i] << "; should end after "
             << lend << endl;
        board_dump();
      }
      lend = cascade_divs[i];
    }
    
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) {
      if (reserve[i].value && !reserve[i].face) {
        cerr << "Bad card at reserve[" << i << "]: "
             << (int) reserve[i].value << endl;
        abort();
      }
    }
    for (card_count_t i = 0; i < CARD_BANK_SIZE; ++i) {
      if (cards[i].value && !cards[i].face) {
        cerr << "Bad card at index " << i << ": "
             << (int) cards[i].value << endl;
        abort();
      }
    }
    
    card_count_t rsz = 0, fsz = 0, bsz = 0;
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) if (reserve[i]) ++rsz;
    for (card_count_t i = 0; i < 4; ++i) fsz += foundation[i];
    for (card_count_t i = 0; i < CARD_BANK_SIZE; ++i) if (cards[i]) ++bsz;
    const card_count_t cc = rsz + fsz + bsz;
    if (cc != TOTAL_CARDS) {
      cerr << "There are somehow " << (int) cc << " cards on the current board"
           << " (" << (int) bsz << " cards on the board, " << (int) rsz
           << " in the reserve, " << (int) fsz << " in the foundation)" << endl;
      board_dump();
    }
    
    for (int i = 0; i < CASCADE_COUNT; ++i) {
      CascadeView c = cascade(i);
      for (card_count_t j = 0; j < c.size; ++j) {
        if (!c.card[j]) {
          cerr << "Bad card data inside bounds of cascade " << i << "!" << endl;
          board_dump();
        }
      }
    }
  }
  
  void reserve_card(Card c) {
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) if (!reserve[i]) {
      reserve[i] = c;
      return;
    }
  }
  
  bool is_won() const {
    for (int i = 0; i < 4; ++i) {
      if (foundation[i] < Card::Face::K * NUM_DECKS) return false;
    }
    return true;
  }
  
  int completion() const {
    return (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * 100 / 52;
  }
  
  FluffyBoard inflate() const {
    FluffyBoard res;
    for (int i = 0; i < 4; ++i) {
      if (foundation[i]) {
        int face = foundation[i] % Card::Face::K ?: Card::Face::K;
        res.foundation[i] = Card((Card::Face) face, (Card::Suit) i);
      } else {
        res.foundation[i].clear();
      }
    }
    {
      size_t i = 0;
      for (card_count_t c : cascade_divs) {
        FluffyCascade fc;
        while (i < c) fc.push_back(cards[i++]);
        res.cascades.push_back(fc);
        ++i;
      }
    }
    for (size_t i = 0; i < RESERVE_SIZE; ++i) {
      res.reserve[i] = reserve[i];
    }
    return res;
  }
  
  Board() {}
  Board(const FluffyBoard &b) {
    int total_cards = 0;
    for (card_count_t i = 0; i < 4; ++i) {
      if (b.foundation[i].value &&
          (b.foundation[i].suit != i || !b.foundation[i].face)) {
        cerr << "Invalid board state. Bad foundation."
             << " (Slot " << (int) i << ":  Suit=" << (int) b.foundation[i].suit
             << ", Face=" << (int) b.foundation[i].face << ")";
        abort();
      }
      foundation[i] = b.foundation[i].face;
      total_cards += foundation[i];
    }
    for (card_count_t i = 0; i < RESERVE_SIZE; ++i) {
      reserve[i] = b.reserve[i];
      if (reserve[i]) ++total_cards;
    }
    if (b.cascades.size() > CASCADE_COUNT) {
      cerr << "Invalid board state. Too many cascades: "
           << "expected " << (int) CASCADE_COUNT << " or fewer; got "
           << b.cascades.size() << endl;
      abort();
    }
    
    /* Compute and check total card count. */ {
      for (card_count_t i = 0; i < CASCADE_COUNT; ++i) {
        total_cards += b.cascades[i].size();
      }
    }
    
    if (total_cards > TOTAL_CARDS) {
      cerr << "Invalid board state. Too many cards: Board contains "
           << (int) total_cards << " cards, but the limit is "
           << (int) TOTAL_CARDS << endl;
      abort();
    }
    
    card_count_t d = 0;
    for (card_count_t i = 0; i < CASCADE_COUNT; ++i) {
      for (Card c : b.cascades[i]) cards[d++] = c;
      cards[d].value = 0;
      cascade_divs[i] = d++;
    }
    check_sanity();
  }
};
using CascadeView = Board::CascadeView;

struct Move {
  enum Place : char { CASCADE = -3, RESERVE = -2, FOUNDATION = -1 };
  int8_t source; ///< Source Place or Card.
  int8_t dest; ///< Destination Place or Card.
  int8_t count; ///< Number of cards to move.
                ///< Must always be 13 or fewer, if not ≤ 5.
  
  string str() const {
    return "Move " + name(source) + " onto " + name(dest);
  }
  
  static const Move kGameStartMove;
  
  Move(Card c, Card d, int8_t count):
      source(c.value), dest(d.value), count(count) {}
  Move(Card c, Place p):
      source(c.value), dest(p), count(1) {}
  Move(Card c, CascadeView d):
      source(c.value), dest(place(d)), count(1) {}
  Move(CascadeView c, CascadeView d, int8_t count):
      source(c.back().value), dest(place(d)), count(count) {}
  Move(CascadeView c, Place p):
      source(c.back().value), dest(p), count(1) {}
  
 private:
  static int8_t place(CascadeView c) {
    if (c.empty()) return Place::CASCADE;
    return c.back().value;
  }
  
  static string name(char place) {
    switch (place) {
      case CASCADE: return "an empty cascade";
      case RESERVE: return "an empty reserve";
      case FOUNDATION: return "the foundation";
    }
    return "the " + Card(place).str();
  }
  
  class GameStart {};
  Move(class GameStart): source(), dest(), count() {}
};

const Move Move::kGameStartMove {Move::GameStart{}};

struct SearchBoard: Board {
  mutable const SearchBoard *previous;
  mutable Move action_taken;
  mutable unsigned depth;
  int heuristic;
  
  int num_moves() const {
    return depth;
  }
  
  int calc_heuristic() const {
    int heur = (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * HEURISTIC_GREED;
    
    card_count_t i = 0;
    for (size_t dno = 0; dno < CASCADE_COUNT; ++dno) {
      card_count_t d = cascade_divs[dno];
      while (++i < d) {
        if (cards[i].face > cards[i-1].face) {
          heur -= (d - i) * INACCESSIBILITY_PUNISHMENT;
        } else {
          heur += TABLEAU_REWARD;
        }
      }
      ++i;
    }
    
    heur -= num_moves() * MOVE_PUNISHMENT;
    return heur;
  }
  
  /** Hashes only the cascades and foundation of a Board. */
  struct Hash {
    uint64_t operator()(const SearchBoard &board) const {
      constexpr size_t block_sz = sizeof(cards) + sizeof(foundation);
      static_assert(block_sz > 16);
      static_assert(100 * sizeof(uint64_t) / sizeof(card_count_t)
                 == sizeof(uint64_t) / sizeof(card_count_t) * 100);
      constexpr size_t chunk_count = block_sz / sizeof(uint64_t);
      constexpr size_t remainder = block_sz % sizeof(uint64_t);
      
      size_t i;
      uint64_t result = 0;
      uint64_t *chunks = (uint64_t*) &board.cards;
      for (i = 0; i < chunk_count; ++i) {
        result ^= chunks[i];
      }
      if (remainder) {
        result ^= ((uint64_t*) &board.cascade_divs) [-1];
      }
      return result;
    }
  };
  
  /** Tests only the cascades and foundation of a Board for equality. */
  struct BasicallyEqual {
    bool operator()(const SearchBoard &a, const SearchBoard &b) const {
      for (card_count_t i = 0; i < CARD_BANK_SIZE; ++i) {
        if (a.cards[i].value != b.cards[i].value) return false;
      }
      for (card_count_t i = 0; i < 4; ++i) {
        if (a.foundation[i] != b.foundation[i]) return false;
      }
      return true;
    }
  };
  
  struct PtrLess {
    bool operator()(const SearchBoard *a, const SearchBoard *b) const {
      return a->heuristic < b->heuristic;
    }
  };
  
  bool operator<(const SearchBoard& other) const {
    return heuristic < other.heuristic;
  }

  SearchBoard(const SearchBoard *prev, const Move &o_move):
      Board(), previous(prev), action_taken(o_move),
      depth(prev->depth + 1), heuristic(0) {}
  SearchBoard(const Board& board, const SearchBoard *prev, const Move &o_move):
      Board(board), previous(prev), action_taken(o_move),
      depth(prev->depth + 1), heuristic(0) {}
  SearchBoard(const Board& board):
      Board(board), previous(nullptr), action_taken(Move::kGameStartMove),
      depth(0), heuristic(0) {}
};

struct MoveDescription {
  string action;
  FluffyBoard result;
  
  MoveDescription(const Move &move, const Board &board):
      action(move.str()), result(board.inflate()) {}
};

typedef vector<MoveDescription> MoveList;
MoveList describeMoves(const SearchBoard &winning_board) {
  MoveList res;
  res.reserve(winning_board.num_moves());
  for (const SearchBoard *b = &winning_board; b->previous; b = b->previous) {
    res.push_back(MoveDescription {b->action_taken, *b});
  }
  std::reverse(res.begin(), res.end());
  return res;
}


// =============================================================================
// === Search Logic ============================================================
// =============================================================================

std::unordered_set<SearchBoard, SearchBoard::Hash, SearchBoard::BasicallyEqual>
typedef MoveGraph;

template<bool weights> struct SQT {
  using T = const SearchBoard*;
  struct SearchQ: priority_queue<T, std::vector<T>, SearchBoard::PtrLess> {
    void pop_back() { c.pop_back(); }
  };
};
template<> struct SQT<false> {
  struct SearchQ: std::queue<const SearchBoard*> {
    const SearchBoard *top() { return front(); }
    void pop_back() { c.pop_back(); }
  };
};

SQT<HEURISTIC_GREED || INACCESSIBILITY_PUNISHMENT || TABLEAU_REWARD>::SearchQ
typedef SearchQueue;

struct AvailableMove {
  Move move;
  Board board;
};

// End of type declarations.

bool tableau_stackable(Card btm, Card top) {
  if (!top) return false;
  return (btm.face == top.face + 1 && btm.color() != top.color()) || !btm.value;
}

bool foundation_can_accept(const Board& b, Card c) {
  return c && (b.foundation[c.suit] + 1) % Card::Face::K == c.face % Card::Face::K;
}

bool reserve_to_tableau_valid(const Board& b, int reserve, size_t cascade) {
  return tableau_stackable(b.cascade_back(cascade), b.reserve[reserve]);
}

SearchBoard reserve_to_tableau(const SearchBoard &b, int reserve, int cascade) {
  SearchBoard res { &b, Move(b.reserve[reserve], b.cascade(cascade)) };
  res.copy_from_and_append(b, cascade, b.reserve[reserve]);
  res.reserve[reserve].value = 0;
  return res;
}

bool tableaux_move_valid(const Board& b, int source, int dest) {
  // tableau_stackable trivially returns true if dest is empty, and will still
  // return false if the source is empty because the zero card will not be an
  // increase in value over whatever the contents of dest are.
  return tableau_stackable(b.cascade_back(dest), b.cascade_back(source));
}

SearchBoard tableaux_move(const SearchBoard &b, size_t source, size_t dest) {
  SearchBoard res { &b, Move(b.cascade(source), b.cascade(dest), 1) };
  res.copy_from_but_move(b, source, dest, 1);
  return res;
}

SearchBoard tableau_to_reserve(const SearchBoard &b, card_count_t source) {
  SearchBoard res { &b, Move(b.cascade(source), Move::Place::RESERVE) };
  res.copy_from_but_remove(b, source);
  res.reserve_card(b.cascade_back(source));
  return res;
}

SearchBoard tableau_to_foundation(const SearchBoard &b, card_count_t source) {
  SearchBoard res { &b, Move(b.cascade(source), Move::Place::FOUNDATION) };
  res.copy_from_but_remove(b, source);
  res.foundation[b.cascade_back(source).suit]++;
  return res;
}

bool foundation_to_tableau_valid(
    const Board& board, card_count_t foundation, card_count_t cascade) {
  if (!board.foundation[foundation]) return false;
  Card onto = board.cascade_back(cascade);
  if (!onto) return true;
  if (Card::color((Card::Suit) foundation) == onto.color()) return false;
  return board.foundation[foundation] == onto.face - 1;
}

SearchBoard foundation_to_tableau(
    const SearchBoard &b, card_count_t foundation, card_count_t cascade) {
  Card fcard((Card::Face) b.foundation[foundation], (Card::Suit) foundation);
  SearchBoard res { &b, Move(fcard, b.cascade(cascade)) };
  res.copy_from_and_append(b, cascade, fcard);
  --res.foundation[foundation];
  return res;
}

SearchBoard reserve_to_foundation(const SearchBoard &b, size_t reserve) {
  SearchBoard res { b, &b, Move(b.reserve[reserve], Move::Place::FOUNDATION) };
  res.reserve[reserve].value = 0;
  res.foundation[b.reserve[reserve].suit]++;
  return res;
}

void visit(
    vector<const SearchBoard*> &dest, SearchBoard &&board, MoveGraph &graph) {
  board.heuristic = board.calc_heuristic();
# ifdef DEBUG_MODE
    board.check_sanity();
# endif
  auto ins = graph.insert(std::move(board));
  if (ins.second) {
    dest.push_back(&*ins.first);
  } else {
    if (board.previous) {
      if (board.previous->depth + 1 < ins.first->depth) {
        ins.first->depth = board.previous->depth + 1;
        ins.first->previous = board.previous;
      }
    }
  }
}

vector<const SearchBoard*>
possible_moves(const SearchBoard &board, MoveGraph &move_graph) {
  vector<const SearchBoard*> res;
  for (card_count_t i = 0; i < CASCADE_COUNT; ++i) {
    for (card_count_t j = 0; j < RESERVE_SIZE; ++j)  {
      if (reserve_to_tableau_valid(board, j, i)) {
        visit(res, reserve_to_tableau(board, j, i), move_graph);
      }
    }
    
    if (board.cascade_empty(i)) continue;

    for (card_count_t j = 0; j < CASCADE_COUNT; ++j) {
      if (tableaux_move_valid(board, i, j)) {
        visit(res, tableaux_move(board, i, j), move_graph);
      }
    }
    
    if (!board.reserve_full())  {
      visit(res, tableau_to_reserve(board, i), move_graph);
    }
    
    if (foundation_can_accept(board, board.cascade_back(i))) {
      visit(res, tableau_to_foundation(board, i), move_graph);
    }
    
    for (card_count_t j = 0; j < 4; ++j) {
      if (foundation_to_tableau_valid(board, j, i)) {
        visit(res, foundation_to_tableau(board, j, i), move_graph);
      }
    }
  }
  
  for (card_count_t i = 0; i < RESERVE_SIZE; ++i)  {
    if (foundation_can_accept(board, board.reserve[i])) {
      visit(res, reserve_to_foundation(board, i), move_graph);
    }
  }
  
  return res;
}

MoveList solve(Board game) {
  SearchQueue search;
  MoveGraph move_graph;
  
  auto ins = move_graph.insert(SearchBoard { game });
  search.push(&*ins.first);
  
  int bno = 0, ino = 0;
  
  int compp = 0;
  size_t freed_results = 0;
  while (!search.empty()) {
    const SearchBoard &board = *search.top();
    const int comp = board.completion();
    const int nmoves = board.num_moves();
    if (board.is_won()) {
      cout << endl << "Solution found." << endl << endl;
      return describeMoves(board);
    }
    auto moves = possible_moves(board, move_graph);
    search.pop();
    for (auto &move : moves) {
      if (!(++bno & 0xFFFF)) {
        cout << "\nArbitrary board (heuristic=" << move->heuristic << "):\n"
             << move->inflate().str() << endl << endl;
      }
      search.push(std::move(move));
    }
    if (!(ino++ & 0x1FF) || comp > compp) {
      cout << "Searched " << ino << " boards [" << search.size()
           << ":" << move_graph.size() << "]; " << nmoves
           << " moves deep; maybe " << comp << "% complete...\r";
      compp = comp;
    }
    while (search.size() > GC_UPPER_BOUND) {
      search.pop_back();
      ++freed_results;
    }
  }
  if (freed_results){
    cout << endl << "Search space exhausted (but " << freed_results
         << " were collected due to memory limitations)." << endl << endl;
  } else {
    cout << endl << "Search space exhausted." << endl << endl;
  }
  return {};
}


// =============================================================================
// === Presentation Logic ======================================================
// =============================================================================

// This game was so hard, I wrote this program.
const string kSampleGame =
    ": 6C 9S 2H AC JD AS 9C 7H\n"
    ": 2D AD QC KD JC JS 3D 2C\n"
    ": KC TD 7D 9D QD TS 6D 6H\n"
    ": 8S TH 3H KS 2S QS 8C KH\n"
    ": AH JH 7C 8H 5H 8D 5D 3S\n"
    ": 4S TC 4D QH 4C 3C 5C 6S\n"
    ": 9H 4H 5S 7S";

int usage(int status, const char* prg) {
  cout << "Usage: " << prg << " <game_file>"
          " [--interactive] [--print_boards]\n" << endl;
  cout << "Game file should look something like this:" << endl;
  cout << kSampleGame << endl << endl;
  cout << "Note that the colons are optional, but the game data isn't.\n"
      "You may use numbers in place of 'A', 'T', 'J', 'Q', and 'K'." << endl;
  return status;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return usage(0, *argv);
  }
  
  string fname;
  bool interactive = false;
  bool print_boards = false;
  
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      string arg = argv[i] + 1 + (argv[i][1] == '-');
      if (arg == "interactive") { interactive = true; continue; }
      if (arg == "print_boards") { print_boards = true; continue; }
      cerr << "Unknown flag `" << argv[i] << "'" << endl;
      return usage(1, *argv);
    } else {
      if (fname.empty()) fname = argv[i];
    }
  }
  
  cout << "Parsing board from \"" << fname << "\"..." << endl;
  std::ifstream game_file(fname);
  if (!game_file) {
    cerr << "Failed to open input file." << endl;
    return 2;
  }
  string game_desc { std::istreambuf_iterator<char>(game_file),
                     std::istreambuf_iterator<char>() };
  cout << "Read the following game descriptor:" << endl << game_desc;
  
  FluffyBoard parsed_game { game_desc };
  cout << "Evaluates as the following board:" << endl << parsed_game.str()
       << endl << endl;
  
  MoveList winning_moves = solve(Board { parsed_game });
  
  if (!interactive || !kUseCurses) {
    for (const MoveDescription &move : winning_moves) {
      if (interactive || print_boards) {
        cout << (interactive ? "\n" : "\n\n")
             << move.result.str()
             << endl;
      }
      cout << move.action << endl;
      if (interactive) std::cin.get();
    }
  } else if (winning_moves.size()) {
#   ifndef USE_CURSES
      cerr << "Logic error." << endl; abort();
#   else
      setlocale(LC_ALL, "");
      initscr(); cbreak(); noecho();
      nonl();
      intrflush(stdscr, FALSE);
      keypad(stdscr, TRUE);
      curs_set(FALSE);
      
      int boardwidth = 8 + 7 + 8, boardheight = 0, textwidth = 0;
      for (const MoveDescription &move : winning_moves) {
        int hgt = 0;
        for (const FluffyCascade &c : move.result.cascades) {
          hgt = std::max(hgt, (int) c.size());
        }
        boardheight = std::max(hgt, boardheight );
        boardwidth  = std::max((int) move.result.cascades.size(), boardwidth);
        textwidth   = std::max((int) move.action.length(), textwidth);
      }
      boardheight += 2;
      
      for (auto at = winning_moves.begin(); ; ) {
        clear();
        if (at == winning_moves.end()) {
          string inst = "Press 'Q' to quit.";
          mvaddstr(boardheight + 2, (COLS - inst.length()) / 2, inst.c_str());
        } else {
          const MoveDescription& move = *at;
          string board_str = move.result.str();
          std::basic_string<unsigned int> w(board_str.begin(), board_str.end());
          
          int pcs = 0;
          for (size_t ln = 1, f = 0, nl = board_str.find('\n');;
               ++ln, f = nl + 1, nl = board_str.find('\n', f)) {
            string substr = board_str.substr(f, nl - f);
            mvwprintw(stdscr, ln, (COLS - boardwidth) / 2, substr.c_str());
            ++pcs;
            if (nl == string::npos) break;
          }
          string inst = move.action;
          mvaddstr(boardheight + 2, (COLS - inst.length()) / 2, inst.c_str());
        }
        
        int c = getch();
        if (c == KEY_UP || c == KEY_LEFT || c == KEY_BACKSPACE) {
          if (at != winning_moves.begin()) at--;
        }
        else if (c == KEY_DOWN || c == KEY_RIGHT || c == KEY_ENTER || c == ' ') {
          if (at != winning_moves.end()) at++;
        }
        else if (c == 'Q' || c == 'q') break;
      }
      endwin();
#   endif
  }
  
  if (winning_moves.empty()) {
    cerr << "Solution could not be found." << endl;
    return 1;
  }
  return 0;
}
