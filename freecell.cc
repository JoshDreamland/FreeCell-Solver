#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#ifdef USE_CURSES
constexpr bool kUseCurses = true;
#include <ncurses.h>
#else
constexpr bool kUseCurses = false;
#endif

using std::cerr;
using std::cout;
using std::deque;
using std::endl;
using std::priority_queue;
using std::string;
using std::vector;

template<typename... K>
using u_set = std::unordered_set<K...>;

constexpr size_t RESERVE_SIZE = 3; // Number of reserve slots.
constexpr size_t GC_UPPER_BOUND = 1200000; // Maximum search space.

// Heuristic weights
constexpr size_t HEURISTIC_GREED = 32;
constexpr size_t MOVE_PUNISHMENT = 8;
constexpr size_t INACCESSIBILITY_PUNISHMENT = 64;
constexpr size_t TABLEAU_REWARD = 4;

// HEURISTIC_GREED
//   Heuristic bias toward number of cards in the foundation.
//   Smaller values consume more resource, but produce better solutions.
//
// MOVE_PUNISHMENT
//   Heuristic deduction for each move taken. Higher values produce shorter
//   solutions, at the cost of widening the search space.
//
// INACCESSIBILITY_PUNISHMENT
//   Heuristic cost of having high-value cards stacked on top of low-value
//   cards. Higher values will make the play more human-like, for better or
//   for worse.
//
// TABLEAU_REWARD
//   Complementary to INACCESSIBILITY_PUNISHMENT, this value rewards having
//   larger stacks of decreasing card ranks.

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
  uint8_t suit : 2;
  uint8_t face : 5;
  
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
  
  bool operator==(const Card& c) const {
    return suit == c.suit && face == c.face;
  }
  
  char serialize() const {
    return suit * 16 + face;
  }
  
  static Card deserialize(char c) {
    return Card((Face) (c & 15), (Suit) (c / 16));
  }
  
  Card(): suit(0), face(0) {}
  Card(Face f, Suit s): suit(s), face(f) {}
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

typedef deque<Card> Cascade;

struct Board {
  vector<Cascade> cascades {};
  vector<Card> reserve {};
  int foundation[4] {};
  
  bool is_won() const {
    for (size_t i = 0; i < 4; ++i) {
      if (foundation[i] < Card::Face::K) return false;
    }
    return true;
  }
  
  int completion() const {
    return (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * 100 / 52;
  }
  
  string serialize() const {
    size_t len = 4 + cascades.size();
    for (const Cascade &c : cascades) len += c.size();
    string res(len, 0);
    
    size_t i;
    for (i = 0; i < 4; ++i) res[i] = foundation[i];
    for (const Cascade &c : cascades) {
      res[i++] = c.size();
      for (Card r : c) res[i++] = r.serialize();
    }
    return res;
  }
  
  static
  Board deserialize(const string& str, const char (&reserve)[RESERVE_SIZE]) {
    Board res;
    // res.originating_move = &kDeserealizedBoardMove;
    if (str.length() < 4) return res;
    
    size_t i;
    for (i = 0; i < 4; ++i)  res.foundation[i] = str[i];
    while (i < str.length()) {
      char count = str[i++];
      res.cascades.push_back({});
      for (int j = 0; j < count; ++j) {
        if (i >= str.length()) abort();
        res.cascades.back().push_back(Card::deserialize(str[i++]));
      }
    }
    for (size_t r = 0; r < RESERVE_SIZE; ++r) {
      if (reserve[r]) {
        res.reserve.push_back(Card::deserialize(reserve[r]));
      }
    }
    
    return res;
  }
  
  string desc() const {
    string res;
    bool more = true;
    for (size_t i = 0; more; ++i) {
      string line = ":";
      more = false;
      for (const Cascade &c : cascades) {
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
      res += (i < reserve.size() ? reserve[i] : Card()).chr() + " ";
    }
    res += "       ";
    for (size_t i = 0; i < 4; ++i) {
      res += Card((Card::Face) foundation[i], (Card::Suit) i).chr() + " ";
    }
    res += "\n\n";
    
    bool more = true;
    for (size_t i = 0; more; ++i) {
      string line;
      more = false;
      for (const Cascade &c : cascades) {
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
  
  Board() {}
  Board(string desc) {
    size_t cascade = 0;
    std::map<char, int> read_cards;
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
      ++read_cards[card.serialize()];
    }
    if (read_cards.size() != 52) {
      cerr << "WARNING: input does not contain all 52 card faces." << endl;
      for (int s = 0; s < 4; ++s) {
        for (int f = Card::Face::A; f <= Card::Face::K; ++f) {
          Card card((Card::Face) f, (Card::Suit) s);
          if (read_cards.find(card.serialize()) == read_cards.end()) {
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
              cerr << "  > " << Card::deserialize(cn.first).str() << endl;
            }
          }
        }
      }
    }
  }
};

struct MoveStub {
  enum Place : char { CASCADE = -3, RESERVE = -2, FOUNDATION = -1 };
  int8_t source; ///< Source Place or Card.
  int8_t dest; ///< Destination Place or Card.
  int8_t count; ///< Number of cards to move.
  
  MoveStub(Card c, Card d, int8_t count):
      source(c.serialize()), dest(d.serialize()), count(count) {}
  MoveStub(Card c, Place p, int8_t count):
      source(c.serialize()), dest(p), count(count) {}
  MoveStub(Card c, const Cascade &d, int8_t count):
      source(c.serialize()), dest(place(d)), count(count) {}
  MoveStub(const Cascade &c, const Cascade &d, int8_t count):
      source(c.back().serialize()), dest(place(d)), count(count) {}
  MoveStub(const Cascade &c, Place p, int8_t count):
      source(c.back().serialize()), dest(p), count(count) {}
  
  string str() const {
    return "Move " + name(source) + " onto " + name(dest);
  }
  
 private:
  static char place(const Cascade &c) {
    if (c.empty()) return Place::CASCADE;
    return c.back().serialize();
  }
  
  static string name(char place) {
    switch (place) {
      case CASCADE: return "an empty cascade";
      case RESERVE: return "an empty reserve";
      case FOUNDATION: return "the foundation";
    }
    return "the " + Card::deserialize(place).str();
  }
};

struct Move: MoveStub {
  char reserve[RESERVE_SIZE] {};
  unsigned depth;
  typedef std::pair<const string, Move> GraphEntry;
  GraphEntry *previous;
  
  static const string kSentinelString;

  struct GameStart {};
  Move(GameStart): MoveStub(Card{}, Card{}, 0), depth(0) , previous(nullptr){}
  Move(GraphEntry &p, const vector<Card> &r, const MoveStub &m):
      MoveStub(m), depth(p.second.depth + 1), previous(&p){
    for (size_t i = 0; i < r.size(); ++i) reserve[i] = r[i].serialize();
  }
};
const string Move::kSentinelString {4, 0};
static Move kGameStartMove { Move::GameStart{} };
using GraphEntry = Move::GraphEntry;

struct SearchBoard: Board {
  GraphEntry *originating_move;
  int depth;
  
  int num_moves() const {
    return depth;
  }
  
  int heuristic() const {
    int heur = (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * HEURISTIC_GREED;
    
    for (const Cascade &c: cascades) {
      for (size_t i = 1; i < c.size(); ++i) {
        if (c[i].face > c[i-1].face) {
          heur -= (1 + c.size() - i) * INACCESSIBILITY_PUNISHMENT;
        } else {
          heur += TABLEAU_REWARD;
        }
      }
    }
    
    heur -= num_moves() * MOVE_PUNISHMENT;
    return heur;
  }
  
  bool operator<(const SearchBoard& other) const {
    return heuristic() < other.heuristic();
  }

  SearchBoard(const Board &board, GraphEntry &originating_move):
      Board(board), originating_move(&originating_move), depth(originating_move.second.depth) {}
  SearchBoard(const Board &&board, GraphEntry &originating_move):
      Board(board), originating_move(&originating_move), depth(originating_move.second.depth) {}
};

struct MoveDescription {
  string action;
  Board result;
  
  MoveDescription(const Move* move, const string &board): MoveDescription(
      *NONNULL(move), board) {}
  MoveDescription(const Move &move, const string &board):
      action(move.str()), result(Board::deserialize(board, move.reserve)) {}
};

typedef vector<MoveDescription> MoveList;
MoveList describeMoves(const SearchBoard &winning_board) {
  MoveList res;
  res.reserve(winning_board.num_moves());
  
  string won_str = winning_board.serialize();
  const string *board_str = &won_str;
  
  for (const Move *mv = &winning_board.originating_move->second; mv->previous;) {
    res.push_back(MoveDescription {mv, *board_str});
    board_str = &mv->previous->first;
    mv = &mv->previous->second;
  }
  
  std::reverse(res.begin(), res.end());
  return res;
}


// =============================================================================
// === Search Logic ============================================================
// =============================================================================

typedef std::unordered_map<string, Move> MoveGraph;

template<bool weights> struct SQT {
  struct SearchQ: priority_queue<SearchBoard> {
    void pop_back() { c.pop_back(); }
  };
};
template<> struct SQT<false> {
  struct SearchQ: std::queue<SearchBoard> {
    const SearchBoard &top() { return front(); }
    void pop_back() { c.pop_back(); }
  };
};

SQT<HEURISTIC_GREED || INACCESSIBILITY_PUNISHMENT || TABLEAU_REWARD>::SearchQ
typedef SearchQueue;

struct AvailableMove {
  MoveStub move;
  Board board;
};

// End of type declarations.

bool tableau_stackable(Card c, Card on_c) {
  return c.face == on_c.face + 1 && c.color() != on_c.color();
}

bool foundation_can_accept(const Board& b, Card c) {
  return b.foundation[c.suit] == c.face - 1;
}

bool reserve_to_tableau_valid(const Board& b, size_t reserve, size_t cascade) {
  if (b.cascades[cascade].empty()) return true;
  return tableau_stackable(b.cascades[cascade].back(), b.reserve[reserve]);
}

AvailableMove
reserve_to_tableau(const Board& b, size_t reserve, size_t cascade) {
  AvailableMove res { MoveStub(b.reserve[reserve], b.cascades[cascade], 1), b };
  res.board.reserve.erase(res.board.reserve.begin() + reserve);
  res.board.cascades[cascade].push_back(b.reserve[reserve]);
  return res;
}

bool tableaux_move_valid(const Board& b, size_t source, size_t dest) {
  if (b.cascades[source].empty()) return false;
  if (b.cascades[dest].empty()) return true;
  return tableau_stackable(b.cascades[dest].back(), b.cascades[source].back());
}

AvailableMove tableaux_move(const Board& b, size_t source, size_t dest) {
  AvailableMove res { MoveStub(b.cascades[source], b.cascades[dest], 1), b };
  res.board.cascades[source].pop_back();
  res.board.cascades[dest].push_back(b.cascades[source].back());
  return res;
}

AvailableMove tableau_to_reserve(const Board& b, size_t source) {
  AvailableMove res {
      MoveStub(b.cascades[source], Move::Place::RESERVE, 1), b };
  res.board.cascades[source].pop_back();
  res.board.reserve.push_back(b.cascades[source].back());
  return res;
}

AvailableMove tableau_to_foundation(const Board& b, size_t source) {
  AvailableMove res {
      MoveStub(b.cascades[source], Move::Place::FOUNDATION, 1), b };
  res.board.cascades[source].pop_back();
  res.board.foundation[b.cascades[source].back().suit]++;
  return res;
}

bool foundation_to_tableau_valid(
    const Board& board, size_t foundation, size_t cascade) {
  if (!board.foundation[foundation]) return false;
  if (board.cascades[cascade].empty()) return true;
  Card onto = board.cascades[cascade].back();
  if (Card::color((Card::Suit) foundation) == onto.color()) return false;
  return board.foundation[foundation] == onto.face - 1;
}

AvailableMove
foundation_to_tableau(const Board& b, size_t foundation, size_t cascade) {
  Card fcard((Card::Face) b.foundation[foundation], (Card::Suit) foundation);
  AvailableMove res { MoveStub(fcard, b.cascades[cascade], 1), b };
  --res.board.foundation[foundation];
  res.board.cascades[cascade].push_back(fcard);
  return res;
}

AvailableMove reserve_to_foundation(const Board& b, size_t reserve) {
  AvailableMove res {
      MoveStub(b.reserve[reserve], Move::Place::FOUNDATION, 1), b };
  if (reserve >= b.reserve.size()) abort();
  res.board.reserve.erase(res.board.reserve.begin() + reserve);
  res.board.foundation[b.reserve[reserve].suit]++;
  return res;
}

void visit(vector<SearchBoard> &dest,
    GraphEntry &p, AvailableMove &&m, MoveGraph &graph) {
  auto ins = graph.insert(GraphEntry{ m.board.serialize(), kGameStartMove });
  if (ins.second) {
    ins.first->second = Move{p, m.board.reserve, m.move };
    dest.push_back(SearchBoard{std::move(m.board), *ins.first});
  } else {
    if (p.second.depth + 1 < ins.first->second.depth) {
      ins.first->second = Move{p, m.board.reserve, m.move };
    }
  }
}

vector<SearchBoard> possible_moves(
    const SearchBoard &board, MoveGraph &move_graph) {
  vector<SearchBoard> res;
  auto &src_move = *board.originating_move;
  for (size_t i = 0; i < board.cascades.size(); ++i) {
    for (size_t j = 0; j < board.reserve.size(); ++j)  {
      if (reserve_to_tableau_valid(board, j, i)) {
        visit(res, src_move, reserve_to_tableau(board, j, i), move_graph);
      }
    }
    
    if (board.cascades[i].empty()) continue;

    for (size_t j = 0; j < board.cascades.size(); ++j) {
      if (tableaux_move_valid(board, i, j)) {
        visit(res, src_move, tableaux_move(board, i, j), move_graph);
      }
    }
    
    if (board.reserve.size() < RESERVE_SIZE)  {
      visit(res, src_move, tableau_to_reserve(board, i), move_graph);
    }
    
    if (foundation_can_accept(board, board.cascades[i].back())) {
      visit(res, src_move, tableau_to_foundation(board, i), move_graph);
    }
    
    for (size_t j = 0; j < 4; ++j) {
      if (foundation_to_tableau_valid(board, j, i)) {
        visit(res, src_move, foundation_to_tableau(board, j, i), move_graph);
      }
    }
  }
  
  for (size_t i = 0; i < board.reserve.size(); ++i)  {
    if (foundation_can_accept(board, board.reserve[i])) {
      visit(res, src_move, reserve_to_foundation(board, i), move_graph);
    }
  }
  
  return res;
}

MoveList solve(Board game) {
  SearchQueue search;
  MoveGraph move_graph;
  
  auto ins = move_graph.insert(GraphEntry(game.serialize(), kGameStartMove));
  search.push({ game, *ins.first });
  
  int bno = 0, ino = 0;
  
  int compp = 0;
  while (!search.empty()) {
    const SearchBoard &board = search.top();
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
        cout << endl << "Arbitrary board:" << endl << move.str() << endl << endl;
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
    }
  }
  cout << endl << "Search space exhausted." << endl << endl;
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
  
  Board game { game_desc };
  cout << "Evaluates as the following board:" << endl << game.str()
       << endl << endl;
  
  MoveList winning_moves = solve(game);
  
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
        for (const Cascade& c : move.result.cascades) {
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
