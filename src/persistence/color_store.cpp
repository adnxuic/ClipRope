#include "persistence/color_store.hpp"

namespace cliprope::persistence {
void write_rope_material(sqlite3 *db, std::string_view id, domain::ColorSelection color,
                         const domain::RopeMaterial &material) {
    Statement write(db, "UPDATE ropes SET "
                        "color_value=?,material_natural=?,tone_deep=?,tone_dark=?,tone_mid=?,tone_light=?,"
                        "tone_high=?,texture_seed=? WHERE id=?");
    write.bind(1, color.encode());
    write.bind(2, int(material.natural));
    for (int i = 0; i < 5; ++i)
        write.bind(i + 3, std::int64_t(material.tones[i]));
    write.bind(8, std::int64_t(material.seed));
    write.bind(9, id);
    write.run();
}
std::vector<domain::ColorRGBA8> recent_colors(Database &db) {
    std::lock_guard lock(db.mutex());
    Statement read(db.handle(), "SELECT color FROM recent_colors ORDER BY rank LIMIT 8");
    std::vector<domain::ColorRGBA8> colors;
    while (read.step())
        if (auto value = domain::parse_color(read.text(0)))
            colors.push_back(*value);
    return colors;
}
void remember_color(sqlite3 *db, domain::ColorSelection color) {
    if (color.is_default())
        return;
    const auto encoded = color.encode();
    std::vector<std::string> colors{encoded};
    {
        Statement read(db, "SELECT color FROM recent_colors ORDER BY rank");
        while (read.step())
            if (auto value = read.text(0); value != encoded && colors.size() < 8)
                colors.push_back(value);
    }
    exec(db, "DELETE FROM recent_colors");
    for (size_t i = 0; i < colors.size(); ++i) {
        Statement write(db, "INSERT INTO recent_colors VALUES(?,?)");
        write.bind(1, colors[i]);
        write.bind(2, int(i));
        write.run();
    }
}
} // namespace cliprope::persistence
