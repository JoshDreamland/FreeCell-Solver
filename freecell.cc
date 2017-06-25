#include <cstdlib>
#include <deque>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

using std::cerr;
using std::cout;
using std::deque;
using std::endl;
using std::priority_queue;
using std::string;
using std::unordered_set;
using std::vector;

constexpr size_t RESERVE_SIZE = 4;

struct Card {
  enum Suit { SPADE, HEART, DIAMOND, CLUB } suit {};
  enum Face {
    EMPTY, A, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN, J, Q, K
  } face {};
  
  string str() const {
    static string names[] = {
      "ERR", "Ace", "One", "Two", "Three", "Four", "Five", "Six", "Seven",
      "Eight", "Nine", "Ten", "Jack", "Queen", "King" 
    };
    static string suits[] = { "Spades", "Hearts", "Diamonds", "Clubs" };
    if (!face) return "Empty";
    return names[face] + " of " + suits[suit];
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
  
  Card() {}
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
typedef vector<string> MoveList;

struct Board {
  vector<Cascade> cascades {};
  vector<Card> reserve {};
  int foundation[4] {};
  MoveList moves {};
  
  string tableau(size_t ind) const {
    if (cascades[ind].empty()) {
      return "Empty Cascade";
    }
    return cascades[ind].back().str();
  }
  
  bool is_won() const {
    for (const Cascade &c : cascades) {
      if (c.size()) return false;
    }
    return reserve.empty();
  }
  
  int heuristic() const {
    return foundation[0] + foundation[1] + foundation[2] + foundation[3];
  }
  
  bool operator<(const Board& other) const {
    return heuristic() < other.heuristic();
  }
  
  bool operator==(const Board& b) const;
  
  unordered_set<Card> reserve_set() const;
  
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
  
  Board() {}
  Board(string desc) {
    size_t cascade = 0;
    for (size_t i = 0; i < desc.length(); ) {
      while (i < desc.length() && std::isspace(desc[i])) ++i;
      if (i >= desc.length()) break;
      
      if (desc[i] == ':' || desc[i] == '\r' || desc[i] == '\n') {
        cascade = 0; ++i;
        continue;
      }
      
      const size_t f = i;
      while (i < desc.length() && !std::isspace(desc[i]) && desc[i] != ':') ++i;
      string card_desc = desc.substr(f, i - f);
      if (cascade >= cascades.size()) cascades.push_back({});
      cascades[cascade++].push_back(Card(card_desc));
    }
  }
};


namespace std {
  template<> struct hash<::Card> {
    typedef ::Card argument_type;
    typedef char result_type;
    result_type operator()(argument_type const& c) const {
      return c.suit << 4 | c.face;
    }
  };
  
  template<> struct hash<deque<::Card>> {
    typedef deque<::Card> argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& vc) const {
      result_type res = 0;
      for (const ::Card &c : vc) {
        res = (res << 7) | (res >> 57);
        res ^= c.suit << 4 | c.face;
      }
      return res;
    }
  };

  template<typename T> struct hash<vector<T>> {
    typedef vector<T> argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& vt) const {
      result_type res = 0;
      std::hash<T> ehasher;
      for (const T &t : vt) {
        res = (res << 9) | (res >> 55);
        res ^= ehasher(t);
      }
      return res;
    }
  };
  
  template<> struct hash<::Board> {
    typedef ::Board argument_type;
    typedef std::size_t result_type;
    result_type operator()(argument_type const& b) const {
      result_type h1 = hash<vector<Cascade>>{}(b.cascades);
      result_type h2 = hash<vector<Card>>{}(b.reserve);
      // result_type const h2(std::hash<vector<Card>>{}(b.reserve));
      result_type h3 = b.foundation[0];
      h3 = (h3 << 8) ^ b.foundation[1];
      h3 = (h3 << 8) ^ b.foundation[2];
      h3 = (h3 << 8) ^ b.foundation[3];
      return h1 ^ (h2 << 1) ^ (h3 << 16);
    }
  };
}

unordered_set<Card> Board::reserve_set() const {
  unordered_set<Card> res;
  for (auto& x : reserve)
    res.insert(x);
  return res;
}

bool Board::operator==(const Board& b) const {
  return cascades == b.cascades && reserve_set() == b.reserve_set() && foundation == b.foundation;
}

bool tableau_stackable(Card c, Card on_c) {
  return c.face == on_c.face + 1 && c.color() != on_c.color();
}

bool foundation_can_accept(const Board& b, Card c) {
  if (c.face == Card::Face::A) {
    cout << endl << "Yes! OMG!! This is a 1: " << (b.foundation[c.suit] == c.face - 1) << endl;
  }
  return b.foundation[c.suit] == c.face - 1;
}

Board reserve_to_tableau(const Board& b, size_t reserve, size_t cascade) {
  Board res = b;
  res.cascades[cascade].push_back(b.reserve[reserve]);
  res.reserve.erase(res.reserve.begin() + reserve);
  res.moves.push_back(b.reserve[reserve].str() + " → " + b.tableau(cascade));
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
  res.moves.push_back(b.tableau(source) + " → " + b.tableau(dest));
  return res;
}

Board tableau_to_reserve(const Board& b, size_t source) {
  Board res = b;
  res.cascades[source].pop_back();
  res.reserve.push_back(b.cascades[source].back());
  res.moves.push_back(b.tableau(source) + " → Empty Resrve");
  return res;
}

Board tableau_to_foundation(const Board& b, size_t source) {
  Board res = b;
  res.cascades[source].pop_back();
  res.foundation[b.cascades[source].back().suit]++;
  res.moves.push_back(b.tableau(source) + " → Foundation");
  return res;
}

Board reserve_to_foundation(const Board& b, size_t reserve) {
  Board res = b;
  res.reserve.erase(res.reserve.begin() + reserve);
  res.foundation[b.cascades[reserve].back().suit]++;
  res.moves.push_back(b.reserve[reserve].str() + " → Foundation");
  return res;
}

vector<Board> possible_moves(const Board &board) {
  vector<Board> res;
  for (size_t i = 0; i < board.cascades.size(); ++i) {
    for (size_t j = 0; j < board.reserve.size(); ++j)  {
      res.push_back(reserve_to_tableau(board, j, i));
    }
    
    if (board.cascades[i].empty()) continue;

    for (size_t j = 0; j < board.cascades.size(); ++j) {
      if (tableaux_move_valid(board, i, j)) {
        res.push_back(tableaux_move(board, i, j));
      }
    }
    
    if (board.reserve.size() < RESERVE_SIZE)  {
      res.push_back(tableau_to_reserve(board, i));
    }
    
    if (foundation_can_accept(board, board.cascades[i].back())) {
      res.push_back(tableau_to_foundation(board, i));
    }
  }
  
  for (size_t i = 0; i < board.reserve.size(); ++i)  {
    if (foundation_can_accept(board, board.reserve[i])) {
      res.push_back(reserve_to_foundation(board, i));
    }
  }
  
  return res;
}

MoveList solve(Board game) {
  priority_queue<Board> search;
  search.push(game);
  int bno = 0;
  
  unordered_set<Board> visited_boards;
  
  while (!search.empty()) {
    const Board &board = search.top();
    const int heur = board.heuristic();
    if (board.is_won()) {
      return board.moves;
    }
    auto moves = possible_moves(board);
    search.pop();
    for (auto &move : moves) {
      if (!(++bno & 0xFFFF)) {
        cout << endl << "Arbitrary board:" << endl << move.desc() << endl << endl;
      }
      if (!visited_boards.insert(move).second) {
        cout << "Pruned a previously-visited board from search space!" << endl;
        continue;
      }
      search.push(std::move(move));
    }
    cout << "Searching space of " << search.size() << " boards with top heuristic at " << heur << "...\r";
    if (bno > 300000) break;
  }
  return {};
}


int main() {
  cout << "Parse board..." << endl;
  Board game {
    ": 6C 9S 2H AC JD AS 9C 7H"
    ": 2D AD QC KD JC JS 3D 2C"
    ": KC TD 7D 9D QD TS 6D 6H"
    ": 8S TH 3H KS 2S QS 8C KH"
    ": AH JH 7C 8H 5H 8D 5D 3S"
    ": 4S TC 4D QH 4C 3C 5C 6S"
    ": 9H 4H 5S 7S"
  };
  cout << "Will solve " << game.desc() << endl << endl;
  cout << "C: " << Card("4H").color() << endl;
  cout << "C: " << Card("5S").color() << endl;
  cout << "C: " << Card("6D").color() << endl;
  cout << "C: " << Card("7C").color() << endl;
  cout << "C: " << Card("3H").color() << endl;
  cout << "C: " << Card("4S").color() << endl;
  cout << "C: " << Card("5D").color() << endl;
  cout << "C: " << Card("6C").color() << endl;
  MoveList winning_moves = solve(game);
  for (string move : winning_moves) cout << move << endl;
  if (winning_moves.empty()) {
    cerr << "Solution could not be found." << endl;
    return 1;
  }
  return 0;
}
