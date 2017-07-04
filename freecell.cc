#include <cstdlib>
#include <deque>
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

using std::cerr;
using std::cout;
using std::deque;
using std::endl;
using std::priority_queue;
using std::string;
using std::vector;

template<typename... K>
using u_set = std::unordered_set<K...>;

constexpr size_t RESERVE_SIZE = 4; // Number of reserve slots.
constexpr size_t GC_UPPER_BOUND = 600000; // Maximum search space.
constexpr size_t GC_LOWER_BOUND = 300000; // Search space prune length.
constexpr size_t HEURISTIC_GREED = 12; // Heuristic bias toward foundation count.
                                       // Smaller values consume more resource,
                                       // but produce better solutions. With a
                                       // greed weight of zero, the algorithm
                                       // behaves as if there were no priority.

// =============================================================================
// === Representation ==========================================================
// =============================================================================

struct Card {
  enum Suit { SPADE, HEART, DIAMOND, CLUB } suit {};
  enum Face {
    EMPTY, A, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN, J, Q, K
  } face {};
  
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
    return color(suit);
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
  
  Card() {}
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

struct Move {
  enum Place : char { CASCADE = -3, RESERVE = -2, FOUNDATION = -1 };
  const string& board_state;
  char source;
  char dest;
  char reserve[RESERVE_SIZE] {};
  
  static const string kSentinel;

  string str() const {
    return "Move " + name(source) + " onto " + name(dest);
  }

  Move(Card c, Card d):
      board_state(kSentinel), source(c.serialize()), dest(d.serialize()) {}
  Move(Card c, Place p):
      board_state(kSentinel), source(c.serialize()), dest(p) {}
  Move(Card c, const Cascade &d):
      board_state(kSentinel), source(c.serialize()), dest(place(d)) {}
  Move(const Cascade &c, const Cascade &d):
      board_state(kSentinel), source(c.back().serialize()), dest(place(d)) {}
  Move(const Cascade &c, Place p):
      board_state(kSentinel), source(c.back().serialize()), dest(p) {}
  Move(const string& b, const Move& m):
      board_state(b), source(m.source), dest(m.dest) {
    for (size_t i = 0; i < RESERVE_SIZE; ++i) reserve[i] = m.reserve[i];
  }
  Move(const string& b, const vector<Card> &r, const Move& m):
      board_state(b), source(m.source), dest(m.dest) {
    for (size_t i = 0; i < r.size(); ++i) reserve[i] = r[i].serialize();
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
const string Move::kSentinel {4, 0};

typedef vector<Move> IffyMoveList;
struct MoveList : IffyMoveList {
  u_set<string> board_states;
  MoveList() {}
  MoveList(const IffyMoveList& ml) {
    reserve(ml.size());
    for (const Move& m : ml) {
      auto it = board_states.insert(m.board_state);
      push_back(Move(*it.first, m));
    }
  }
};

struct Board {
  vector<Cascade> cascades {};
  vector<Card> reserve {};
  int foundation[4] {};
  IffyMoveList moves {};
  
  bool is_won() const {
    for (size_t i = 0; i < 4; ++i) {
      if (foundation[i] < Card::Face::K) return false;
    }
    return true;
  }
  
  int heuristic() const {
    return (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * HEURISTIC_GREED - moves.size();
  }
  
  int completion() const {
    return (foundation[0] + foundation[1] + foundation[2] + foundation[3])
        * 100 / 52;
  }
  
  bool operator<(const Board& other) const {
    return heuristic() < other.heuristic();
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

// =============================================================================
// === Search Logic ============================================================
// =============================================================================

template<int greed> struct SQT { typedef priority_queue<Board> SearchQueue; };
template<> struct SQT<0> { struct SearchQueue: std::queue<Board> {
  const Board &top() { return front(); }
}; };
typedef SQT<HEURISTIC_GREED>::SearchQueue SearchQueue;

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

Board reserve_to_tableau(const Board& b, size_t reserve, size_t cascade) {
  Board res = b;
  res.reserve.erase(res.reserve.begin() + reserve);
  res.cascades[cascade].push_back(b.reserve[reserve]);
  res.moves.push_back(Move(b.reserve[reserve], b.cascades[cascade]));
  return res;
}

bool tableaux_move_valid(const Board& b, size_t source, size_t dest) {
  if (b.cascades[source].empty()) return false;
  if (b.cascades[dest].empty()) return true;
  return tableau_stackable(b.cascades[dest].back(), b.cascades[source].back());
}

Board tableaux_move(const Board& b, size_t source, size_t dest) {
  Board res = b;
  res.cascades[source].pop_back();
  res.cascades[dest].push_back(b.cascades[source].back());
  res.moves.push_back(Move(b.cascades[source], b.cascades[dest]));
  return res;
}

Board tableau_to_reserve(const Board& b, size_t source) {
  Board res = b;
  res.cascades[source].pop_back();
  res.reserve.push_back(b.cascades[source].back());
  res.moves.push_back(Move(b.cascades[source], Move::Place::RESERVE));
  return res;
}

Board tableau_to_foundation(const Board& b, size_t source) {
  Board res = b;
  res.cascades[source].pop_back();
  res.foundation[b.cascades[source].back().suit]++;
  res.moves.push_back(Move(b.cascades[source], Move::Place::FOUNDATION));
  return res;
}

Board reserve_to_foundation(const Board& b, size_t reserve) {
  Board res = b;
  if (reserve >= b.reserve.size()) abort();
  res.reserve.erase(res.reserve.begin() + reserve);
  res.foundation[b.reserve[reserve].suit]++;
  res.moves.push_back(Move(b.reserve[reserve], Move::Place::FOUNDATION));
  return res;
}

void visit(vector<Board>& dest, Board&& b, u_set<string> &visited) {
  auto ins = visited.insert(b.serialize());
  if (ins.second) {
    Move fixed(*ins.first, b.reserve, b.moves.back());
    b.moves.pop_back();
    b.moves.push_back(fixed);
    dest.push_back(std::move(b));
  }
}

vector<Board> possible_moves(const Board &board, u_set<string> &visited) {
  vector<Board> res;
  for (size_t i = 0; i < board.cascades.size(); ++i) {
    for (size_t j = 0; j < board.reserve.size(); ++j)  {
      if (reserve_to_tableau_valid(board, j, i)) {
        visit(res, reserve_to_tableau(board, j, i), visited);
      }
    }
    
    if (board.cascades[i].empty()) continue;

    for (size_t j = 0; j < board.cascades.size(); ++j) {
      if (tableaux_move_valid(board, i, j)) {
        visit(res, tableaux_move(board, i, j), visited);
      }
    }
    
    if (board.reserve.size() < RESERVE_SIZE)  {
      visit(res, tableau_to_reserve(board, i), visited);
    }
    
    if (foundation_can_accept(board, board.cascades[i].back())) {
      visit(res, tableau_to_foundation(board, i), visited);
    }
  }
  
  for (size_t i = 0; i < board.reserve.size(); ++i)  {
    if (foundation_can_accept(board, board.reserve[i])) {
      visit(res, reserve_to_foundation(board, i), visited);
    }
  }
  
  return res;
}

MoveList solve(Board game) {
  SearchQueue search;
  search.push(game);
  int bno = 0, ino = 0;
  
  u_set<string> visited_boards;
  
  int compp = 0;
  while (!search.empty()) {
    const Board &board = search.top();
    const int comp = board.completion();
    if (board.is_won()) {
      cout << endl << "Solution found." << endl << endl;
      return board.moves;
    }
    auto moves = possible_moves(board, visited_boards);
    search.pop();
    for (auto &move : moves) {
      if (!(++bno & 0xFFFF)) {
        cout << endl << "Arbitrary board:" << endl << move.str() << endl << endl;
      }
      search.push(std::move(move));
    }
    if (!(ino++ & 0x1FF) || comp > compp) {
      cout << "Searching space of " << search.size()
           << "/" << visited_boards.size() << " boards; maybe " << comp
           << "% done...\r";
      compp = comp;
    }
    if (search.size() > GC_UPPER_BOUND) {
      // This operation is an abomination that exists because priority_queue
      // is grossly underpowered, and I'm too lazy to use to a set.
      cout << endl <<  "Collecting some garbage..." << endl;
     SearchQueue garbage;
      while (garbage.size() < GC_LOWER_BOUND) {
        garbage.push(search.top());
        search.pop();
      }
      search.swap(garbage);
    }
  }
  cout << endl << "Search space exhausted." << endl << endl;
  return {};
}

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
    for (const Move &move : winning_moves) {
      if (interactive || print_boards) {
        cout << (interactive ? "\n" : "\n\n")
             << Board::deserialize(move.board_state, move.reserve).str()
             << endl;
      }
      cout << move.str() << endl;
      if (interactive) std::cin.get();
    }
  } else {
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
      for (const Move &move : winning_moves) {
        int hgt = 0;
        Board b = Board::deserialize(move.board_state, move.reserve);
        for (const Cascade& c : b.cascades) {
          hgt = std::max(hgt, (int) c.size());
        }
        boardheight = std::max(hgt, boardheight );
        boardwidth  = std::max((int) b.cascades.size(), boardwidth);
        textwidth   = std::max((int) move.str().length(), textwidth);
      }
      boardheight += 2;
      
      for (auto at = winning_moves.begin(); ; ) {
        clear();
        if (at == winning_moves.end()) {
          string inst = "Press 'Q' to quit.";
          mvaddstr(boardheight + 2, (COLS - inst.length()) / 2, inst.c_str());
        } else {
          const Move& move = *at;
          string board_str =
              Board::deserialize(move.board_state, move.reserve).str();
          std::basic_string<unsigned int> w(board_str.begin(), board_str.end());
          
          int pcs = 0;
          for (size_t ln = 1, f = 0, nl = board_str.find('\n');;
               ++ln, f = nl + 1, nl = board_str.find('\n', f)) {
            string substr = board_str.substr(f, nl - f);
            mvwprintw(stdscr, ln, (COLS - boardwidth) / 2, substr.c_str());
            ++pcs;
            if (nl == string::npos) break;
          }
          string inst = move.str();
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
