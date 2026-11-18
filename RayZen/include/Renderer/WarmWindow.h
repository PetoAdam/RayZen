#ifndef RAYZEN_RENDERER_WARMWINDOW_H
#define RAYZEN_RENDERER_WARMWINDOW_H

#include <string>

namespace rayzen {

enum class WarmCommand { None, Restore, Quit, Error };

class WarmWindow {
public:
    explicit WarmWindow(std::string scope = {});
    ~WarmWindow();
    WarmWindow(const WarmWindow&) = delete;
    WarmWindow& operator=(const WarmWindow&) = delete;

    bool open();
    WarmCommand poll();
    static bool send(WarmCommand command, const std::string& scope = {});

private:
    int socket_ = -1;
    std::string scope_;
};

} // namespace rayzen

#endif
