#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace hailstone::app {

class CharacterImportUi {
public:
    static constexpr const char* kWindowName = "Import character settings###character_import";
    enum class Outcome { Add, Keep, Skip };

    struct File {
        std::string name;
        std::string label;
    };

    struct Source {
        std::string label;
        std::vector<File> files;
        bool has_actions = false;
    };

    struct Choice {
        std::size_t source = 0;
        bool socials = false;
        bool hotbars = false;
    };

    struct Row {
        std::string slot;
        std::string name;
        std::string reason;
        std::vector<std::string> lines;
        Outcome outcome = Outcome::Skip;
    };

    struct Preview {
        std::vector<Row> rows;
        std::string error;
    };

    struct Result {
        bool ok = false;
        std::string message;
        bool can_restore = false;
    };

    struct Callbacks {
        std::function<Preview(const Choice&)> preview;
        std::function<Result()> apply;
        std::function<Result()> restore;
    };

    void open(std::string target, std::vector<Source> sources, std::string scan_error = {}, bool can_restore = false);
    void close();
    bool visible() const { return open_; }
    void draw(const Callbacks& callbacks);

private:
    enum class Page { Source, Review, Result, Restore };

    const Source* source() const;
    void show(Page page);
    void draw_source();
    void draw_review() const;
    void draw_footer(const Callbacks& callbacks);
    void show_result(Result result);

    bool open_ = false;
    bool reset_scroll_ = false;
    bool can_restore_ = false;
    Page page_ = Page::Source;
    std::string target_;
    std::vector<Source> sources_;
    std::string scan_error_;
    std::string preview_error_;
    Choice choice_;
    Preview preview_;
    Result result_;
    std::size_t additions_ = 0;
    std::size_t matches_ = 0;
};

} // namespace hailstone::app
