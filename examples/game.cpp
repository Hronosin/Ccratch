// game.cpp: C++ as blocks. A namespace, a class, a template, a range-for.
#include <iostream>
#include <string>
#include <vector>

namespace game {

class Player {
public:
    Player(std::string name, int hp) : name_(std::move(name)), hp_(hp) {}
    void hit(int damage) { hp_ -= damage; }
    void heal(int amount)
    {
        hp_ += amount;
        if (hp_ > 100)
            hp_ = 100;
    }
    bool alive() const { return hp_ > 0; }
    const std::string &name() const { return name_; }

private:
    std::string name_;
    int hp_;
};

template <typename T>
T clamp(T v, T lo, T hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

} // namespace game

int main()
{
    std::vector<game::Player> team;
    team.push_back(game::Player("Reimu", 100));
    team.emplace_back("Marisa", 80);
    for (auto &p : team) {
        p.hit(30);
        p.heal(10);
        std::cout << p.name() << (p.alive() ? " lives" : " is gone") << "\n";
    }
    auto *boss = new game::Player("Yukari", 9999);
    int dmg = game::clamp(500, 0, 100);
    boss->hit(dmg);
    delete boss;
    auto twice = [](int x) { return x * 2; };
    std::cout << twice(21) << std::endl;
    return 0;
}
