#include <ui/pattern_drawer.hpp>

#include <pl/core/lexer.hpp>

#include <pl/patterns/pattern_array_dynamic.hpp>
#include <pl/patterns/pattern_array_static.hpp>
#include <pl/patterns/pattern_bitfield.hpp>
#include <pl/patterns/pattern_boolean.hpp>
#include <pl/patterns/pattern_character.hpp>
#include <pl/patterns/pattern_enum.hpp>
#include <pl/patterns/pattern_float.hpp>
#include <pl/patterns/pattern_pointer.hpp>
#include <pl/patterns/pattern_signed.hpp>
#include <pl/patterns/pattern_string.hpp>
#include <pl/patterns/pattern_struct.hpp>
#include <pl/patterns/pattern_union.hpp>
#include <pl/patterns/pattern_unsigned.hpp>
#include <pl/patterns/pattern_wide_character.hpp>
#include <pl/patterns/pattern_wide_string.hpp>

#include <string>

#include <hex/api/imhex_api.hpp>
#include <hex/api/content_registry.hpp>
#include <hex/api/achievement_manager.hpp>
#include <hex/api/localization_manager.hpp>

#include <hex/helpers/utils.hpp>
#include <wolv/math_eval/math_evaluator.hpp>

#include <imgui.h>
#include <hex/ui/imgui_imhex_extensions.h>
#include <fonts/codicons_font.h>

#include <wolv/io/file.hpp>

namespace hex::ui {

    namespace {

        std::mutex s_resetDrawMutex;

        constexpr auto DisplayEndDefault = 50U;

        using namespace ::std::literals::string_literals;

        bool isPatternSelected(u64 address, u64 size) {
            auto currSelection = ImHexApi::HexEditor::getSelection();
            if (!currSelection.has_value())
                return false;

            return Region{ address, size }.overlaps(*currSelection);
        }

        template<typename T>
        auto highlightWhenSelected(u64 address, u64 size, const T &callback) {
            constexpr bool HasReturn = !requires(T t) { { t() } -> std::same_as<void>; };

            auto selected = isPatternSelected(address, size);

            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGuiExt::GetCustomColorVec4(ImGuiCustomCol_PatternSelected));

            if constexpr (HasReturn) {
                auto result = callback();

                if (selected)
                    ImGui::PopStyleColor();

                return result;
            } else {
                callback();

                if (selected)
                    ImGui::PopStyleColor();
            }
        }

        template<typename T>
        auto highlightWhenSelected(const pl::ptrn::Pattern& pattern, const T &callback) {
            return highlightWhenSelected(pattern.getOffset(), pattern.getSize(), callback);
        }

        void drawTypenameColumn(const pl::ptrn::Pattern& pattern, const std::string& pattern_name) {
            ImGuiExt::TextFormattedColored(ImColor(0xFFD69C56), pattern_name);
            ImGui::SameLine();
            ImGui::TextUnformatted(pattern.getTypeName().c_str());
            ImGui::TableNextColumn();
        }

        void drawOffsetColumnForBitfieldMember(const pl::ptrn::PatternBitfieldMember &pattern) {
            if (pattern.isPatternLocal()) {
                ImGuiExt::TextFormatted("[{}]", "hex.ui.pattern_drawer.local"_lang);
                ImGui::TableNextColumn();
                ImGuiExt::TextFormatted("[{}]", "hex.ui.pattern_drawer.local"_lang);
                ImGui::TableNextColumn();
            } else {
                ImGuiExt::TextFormatted("0x{0:08X}, bit {1}", pattern.getOffset(), pattern.getBitOffsetForDisplay());
                ImGui::TableNextColumn();
                ImGuiExt::TextFormatted("0x{0:08X}, bit {1}", pattern.getOffset() + pattern.getSize(), pattern.getBitOffsetForDisplay() + pattern.getBitSize() - (pattern.getSize() == 0 ? 0 : 1));
                ImGui::TableNextColumn();
            }
        }

        void drawOffsetColumn(const pl::ptrn::Pattern& pattern) {
            auto *bitfieldMember = dynamic_cast<pl::ptrn::PatternBitfieldMember const*>(&pattern); 
            if (bitfieldMember != nullptr && bitfieldMember->getParentBitfield() != nullptr) {
                drawOffsetColumnForBitfieldMember(*bitfieldMember);
                return;
            }
            
            if (pattern.isPatternLocal()) {
                ImGuiExt::TextFormatted("[{}]", "hex.ui.pattern_drawer.local"_lang);
            } else {
                ImGuiExt::TextFormatted("0x{0:08X}", pattern.getOffset());
            }

            ImGui::TableNextColumn();

            if (pattern.isPatternLocal()) {
                ImGuiExt::TextFormatted("[{}]", "hex.ui.pattern_drawer.local"_lang);
            } else {
                ImGuiExt::TextFormatted("0x{0:08X}", pattern.getOffset() + pattern.getSize() - (pattern.getSize() == 0 ? 0 : 1));
            }

            ImGui::TableNextColumn();
        }

        void drawSizeColumnForBitfieldMember(const pl::ptrn::PatternBitfieldMember &pattern) {
            if (pattern.getBitSize() == 1)
                ImGuiExt::TextFormatted("1 bit");
            else
                ImGuiExt::TextFormatted("{0} bits", pattern.getBitSize());
        }

        void drawSizeColumn(const pl::ptrn::Pattern& pattern) {
            if (auto *bitfieldMember = dynamic_cast<pl::ptrn::PatternBitfieldMember const*>(&pattern); bitfieldMember != nullptr && bitfieldMember->getParentBitfield() != nullptr)
                drawSizeColumnForBitfieldMember(*bitfieldMember);
            else
                ImGuiExt::TextFormatted("0x{0:04X}", pattern.getSize());

            ImGui::TableNextColumn();
        }

        void drawCommentTooltip(const pl::ptrn::Pattern &pattern) {
            if (auto comment = pattern.getComment(); !comment.empty()) {
                ImGuiExt::InfoTooltip(comment.c_str());
            }
        }

    }

    std::optional<PatternDrawer::Filter> PatternDrawer::parseRValueFilter(const std::string &filter) const {
        Filter result;

        if (filter.empty()) {
            return result;
        }

        result.path.emplace_back();
        for (size_t i = 0; i < filter.size(); i += 1) {
            char c = filter[i];

            if (i < filter.size() - 1 && c == '=' && filter[i + 1] == '=') {
                try {
                    pl::core::Lexer lexer;

                    auto source = filter.substr(i + 2);
                    auto tokens = lexer.lex(filter.substr(i + 2), filter.substr(i + 2));

                    if (!tokens.has_value() || tokens->size() != 2)
                        return std::nullopt;

                    auto literal = std::get_if<pl::core::Token::Literal>(&tokens->front().value);
                    if (literal == nullptr)
                        return std::nullopt;
                    result.value = *literal;
                } catch (pl::core::err::LexerError &) {
                    return std::nullopt;
                }
                break;
            } else if (c == '.')
                result.path.emplace_back();
            else if (c == '[') {
                result.path.emplace_back();
                result.path.back() += c;
            } else if (c == ' ') {
                // Skip whitespace
            } else {
                result.path.back() += c;
            }
        }

        return result;
    }

    bool PatternDrawer::isEditingPattern(const pl::ptrn::Pattern& pattern) const {
        return m_editingPattern == &pattern && m_editingPatternOffset == pattern.getOffset();
    }

    void PatternDrawer::resetEditing() {
        m_editingPattern = nullptr;
        m_editingPatternOffset = 0x00;
    }

    bool PatternDrawer::matchesFilter(const std::vector<std::string> &filterPath, const std::vector<std::string> &patternPath, bool fullMatch) const {
        if (fullMatch) {
            if (patternPath.size() != filterPath.size())
                return false;
        }

        if (patternPath.size() <= filterPath.size()) {
            for (ssize_t i = patternPath.size() - 1; i >= 0; i--) {
                const auto &filter = filterPath[i];

                if (patternPath[i] != filter && !filter.empty() && filter != "*") {
                    return false;
                }
            }
        }

        return true;
    }

    void PatternDrawer::drawFavoriteColumn(const pl::ptrn::Pattern& pattern) {
        if (!m_showFavoriteStars) {
            ImGui::TableNextColumn();
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

        if (m_favorites.contains(m_currPatternPath)) {
            if (ImGuiExt::DimmedIconButton(ICON_VS_STAR_DELETE, ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram))) {
                m_favorites.erase(m_currPatternPath);
            }
        }
        else {
            if (ImGuiExt::DimmedIconButton(ICON_VS_STAR_ADD, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled))) {
                m_favorites.insert({ m_currPatternPath, pattern.clone() });
            }
        }

        ImGui::PopStyleVar();

        ImGui::TableNextColumn();
    }

    void PatternDrawer::drawColorColumn(const pl::ptrn::Pattern& pattern) {
        if (pattern.getVisibility() == pl::ptrn::Visibility::Visible) {
            ImGui::ColorButton("color", ImColor(pattern.getColor()), ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetColumnWidth(), ImGui::GetTextLineHeight()));

            if (m_rowColoring)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, (pattern.getColor() & 0x00'FF'FF'FF) | 0x30'00'00'00);
        }

        ImGui::TableNextColumn();
    }

    void PatternDrawer::drawVisualizer(const std::map<std::string, ContentRegistry::PatternLanguage::impl::Visualizer> &visualizers, const std::vector<pl::core::Token::Literal> &arguments, pl::ptrn::Pattern &pattern, pl::ptrn::IIterable &iterable, bool reset) {
        auto visualizerName = arguments.front().toString(true);

        if (auto entry = visualizers.find(visualizerName); entry != visualizers.end()) {
            const auto &[name, visualizer] = *entry;

            auto paramCount = arguments.size() - 1;
            auto [minParams, maxParams] = visualizer.parameterCount;

            if (paramCount >= minParams && paramCount <= maxParams) {
                try {
                    visualizer.callback(pattern, iterable, reset, { arguments.begin() + 1, arguments.end() });
                } catch (std::exception &e) {
                    m_lastVisualizerError = e.what();
                }
            } else {
                ImGui::TextUnformatted("hex.ui.pattern_drawer.visualizer.invalid_parameter_count"_lang);
            }
        } else {
            ImGui::TextUnformatted("hex.ui.pattern_drawer.visualizer.unknown"_lang);
        }

        if (!m_lastVisualizerError.empty())
            ImGui::TextUnformatted(m_lastVisualizerError.c_str());
    }

    void PatternDrawer::drawValueColumn(pl::ptrn::Pattern& pattern) {
        std::string value;

        try {
            value = pattern.getFormattedValue();
        } catch (const std::exception &e) {
            value = e.what();
        }

        const auto width = ImGui::GetColumnWidth();
        if (const auto &visualizeArgs = pattern.getAttributeArguments("hex::visualize"); !visualizeArgs.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5F));

            bool shouldReset = false;
            if (ImGui::Button(hex::format(" {}  {}", ICON_VS_EYE_WATCH, value).c_str(), ImVec2(width, ImGui::GetTextLineHeight()))) {
                auto previousPattern = m_currVisualizedPattern;

                m_currVisualizedPattern = &pattern;
                m_lastVisualizerError.clear();

                if (m_currVisualizedPattern != previousPattern)
                    shouldReset = true;

                ImGui::OpenPopup("Visualizer");
            }
            ImGui::PopStyleVar(2);

            ImGui::SameLine();

            if (ImGui::BeginPopup("Visualizer")) {
                if (m_currVisualizedPattern == &pattern) {
                    drawVisualizer(ContentRegistry::PatternLanguage::impl::getVisualizers(), visualizeArgs, pattern, dynamic_cast<pl::ptrn::IIterable&>(pattern), !m_visualizedPatterns.contains(&pattern) || shouldReset);
                    m_visualizedPatterns.insert(&pattern);
                }

                ImGui::EndPopup();
            }
        } else if (const auto &inlineVisualizeArgs = pattern.getAttributeArguments("hex::inline_visualize"); !inlineVisualizeArgs.empty()) {
            drawVisualizer(ContentRegistry::PatternLanguage::impl::getInlineVisualizers(), inlineVisualizeArgs, pattern, dynamic_cast<pl::ptrn::IIterable&>(pattern), true);
        } else {
            ImGuiExt::TextFormatted("{}", value);
        }

        if (ImGui::CalcTextSize(value.c_str()).x > width) {
            ImGuiExt::InfoTooltip(value.c_str());
        }
    }

    std::string PatternDrawer::getDisplayName(const pl::ptrn::Pattern& pattern) const {
        if (m_showSpecName && pattern.hasAttribute("hex::spec_name"))
            return pattern.getAttributeArguments("hex::spec_name")[0].toString(true);
        else
            return pattern.getDisplayName();
    }

    bool PatternDrawer::createTreeNode(const pl::ptrn::Pattern& pattern, bool leaf) {
        drawFavoriteColumn(pattern);

        if (pattern.isSealed() || leaf) {
            ImGui::Indent();
            highlightWhenSelected(pattern, [&]{ ImGui::TextUnformatted(this->getDisplayName(pattern).c_str()); });
            ImGui::Unindent();
            return false;
        }

        return highlightWhenSelected(pattern, [&]{
            switch (m_treeStyle) {
                using enum TreeStyle;
                default:
                case Default:
                    return ImGui::TreeNodeEx(this->getDisplayName(pattern).c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                case AutoExpanded:
                    return ImGui::TreeNodeEx(this->getDisplayName(pattern).c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen);
                case Flattened:
                    return ImGui::TreeNodeEx(this->getDisplayName(pattern).c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            }
        });
    }

    void PatternDrawer::makeSelectable(const pl::ptrn::Pattern &pattern) {
        ImGui::PushID(static_cast<int>(pattern.getOffset()));
        ImGui::PushID(pattern.getVariableName().c_str());

        if (ImGui::Selectable("##PatternLine", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
            m_selectionCallback(Region { pattern.getOffset(), pattern.getSize() });

            if (m_editingPattern != &pattern) {
                this->resetEditing();
            }
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_editingPattern = &pattern;
            m_editingPatternOffset = pattern.getOffset();
            AchievementManager::unlockAchievement("hex.builtin.achievement.patterns", "hex.builtin.achievement.patterns.modify_data.name");
        }

        ImGui::SameLine(0, 0);

        ImGui::PopID();
        ImGui::PopID();
    }

    void PatternDrawer::createDefaultEntry(const pl::ptrn::Pattern &pattern) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        createTreeNode(pattern, true);
        ImGui::SameLine(0, 0);
        makeSelectable(pattern);
        drawCommentTooltip(pattern);
        ImGui::TableNextColumn();
        drawColorColumn(pattern);
        drawOffsetColumn(pattern);
        drawSizeColumn(pattern);
        ImGuiExt::TextFormattedColored(ImColor(0xFF9BC64D), "{}", pattern.getFormattedName().empty() ? pattern.getTypeName() : pattern.getFormattedName());
        ImGui::TableNextColumn();
    }

    void PatternDrawer::closeTreeNode(bool inlined) const {
        if (!inlined && m_treeStyle != TreeStyle::Flattened)
            ImGui::TreePop();
    }


    void PatternDrawer::visit(pl::ptrn::PatternArrayDynamic& pattern) {
        drawArray(pattern, pattern, pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternArrayStatic& pattern) {
        drawArray(pattern, pattern, pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternBitfieldField& pattern) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        createTreeNode(pattern, true);
        ImGui::SameLine(0, 0);
        makeSelectable(pattern);
        drawCommentTooltip(pattern);
        ImGui::TableNextColumn();
        drawColorColumn(pattern);
        drawOffsetColumnForBitfieldMember(pattern);
        drawSizeColumnForBitfieldMember(pattern);
        ImGui::TableNextColumn();
        ImGuiExt::TextFormattedColored(ImColor(0xFF9BC64D), "bits");
        ImGui::TableNextColumn();

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        auto value = pattern.getValue();
        auto valueString = pattern.toString();

        if (pattern.getBitSize() == 1) {
            bool boolValue = value.toBoolean();
            if (ImGui::Checkbox("##boolean", &boolValue)) {
                pattern.setValue(boolValue);
            }
        } else if (std::holds_alternative<i128>(value)) {
            if (ImGui::InputText("##Value", valueString, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
                wolv::math_eval::MathEvaluator<i128> mathEvaluator;

                if (auto result = mathEvaluator.evaluate(valueString); result.has_value())
                    pattern.setValue(result.value());

                this->resetEditing();
            }
        } else if (std::holds_alternative<u128>(value)) {
            if (ImGui::InputText("##Value", valueString, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
                wolv::math_eval::MathEvaluator<u128> mathEvaluator;

                if (auto result = mathEvaluator.evaluate(valueString); result.has_value())
                    pattern.setValue(result.value());

                this->resetEditing();
            }
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternBitfieldArray& pattern) {
        drawArray(pattern, pattern, pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternBitfield& pattern) {
        bool open = true;
        if (!pattern.isInlined() && m_treeStyle != TreeStyle::Flattened) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            open = createTreeNode(pattern);
            ImGui::SameLine(0, 0);
            makeSelectable(pattern);
            drawCommentTooltip(pattern);
            ImGui::TableNextColumn();

            if (pattern.isSealed())
                drawColorColumn(pattern);
            else
                ImGui::TableNextColumn();

            drawOffsetColumn(pattern);
            drawSizeColumn(pattern);
            drawTypenameColumn(pattern, "bitfield");

            drawValueColumn(pattern);
        }

        if (!open) {
            return;
        }

        int id = 1;
        pattern.forEachEntry(0, pattern.getEntryCount(), [&] (u64, auto *field) {
            ImGui::PushID(id);
            this->draw(*field);
            ImGui::PopID();

            id += 1;
        });

        closeTreeNode(pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternBoolean& pattern) {
        createDefaultEntry(pattern);

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        bool value = pattern.getValue().toBoolean();
        if (ImGui::Checkbox("##boolean", &value)) {
            pattern.setValue(value);
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternCharacter& pattern) {
        createDefaultEntry(pattern);

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }
            
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        auto value = hex::encodeByteString(pattern.getBytes());
        if (ImGui::InputText("##Character", value.data(), value.size() + 1, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (!value.empty()) {
                auto result = hex::decodeByteString(value);
                if (!result.empty())
                    pattern.setValue(char(result[0]));

                this->resetEditing();
            }
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternEnum& pattern) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        createTreeNode(pattern, true);
        ImGui::SameLine(0, 0);
        makeSelectable(pattern);
        drawCommentTooltip(pattern);
        ImGui::TableNextColumn();
        drawColorColumn(pattern);
        drawOffsetColumn(pattern);
        drawSizeColumn(pattern);
        drawTypenameColumn(pattern, "enum");

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }
    
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        if (ImGui::BeginCombo("##Enum", pattern.getFormattedValue().c_str())) {
            auto currValue = pattern.getValue().toUnsigned();
            for (auto &value : pattern.getEnumValues()) {
                auto min = value.min.toUnsigned();
                auto max = value.max.toUnsigned();

                bool isSelected = min <= currValue && max >= currValue;
                if (ImGui::Selectable(fmt::format("{}::{} (0x{:0{}X})", pattern.getTypeName(), value.name, min, pattern.getSize() * 2).c_str(), isSelected)) {
                    pattern.setValue(value.min);
                    this->resetEditing();
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternFloat& pattern) {
        createDefaultEntry(pattern);

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        auto value = pattern.toString();
        if (ImGui::InputText("##Value", value, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
            wolv::math_eval::MathEvaluator<long double> mathEvaluator;

            if (auto result = mathEvaluator.evaluate(value); result.has_value())
                pattern.setValue(double(result.value()));

            this->resetEditing();
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternPadding& pattern) {
        // Do nothing
        hex::unused(pattern);
    }

    void PatternDrawer::visit(pl::ptrn::PatternPointer& pattern) {
        bool open = true;

        if (!pattern.isInlined() && m_treeStyle != TreeStyle::Flattened) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            open = createTreeNode(pattern);
            ImGui::SameLine(0, 0);
            makeSelectable(pattern);
            drawCommentTooltip(pattern);
            ImGui::TableNextColumn();
            drawColorColumn(pattern);
            drawOffsetColumn(pattern);
            drawSizeColumn(pattern);
            ImGuiExt::TextFormattedColored(ImColor(0xFF9BC64D), "{}", pattern.getFormattedName());
            ImGui::TableNextColumn();
            drawValueColumn(pattern);
        }

        if (open) {
            pattern.getPointedAtPattern()->accept(*this);

            closeTreeNode(pattern.isInlined());
        }
    }

    void PatternDrawer::visit(pl::ptrn::PatternSigned& pattern) {
        createDefaultEntry(pattern);

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }
    
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

        auto value = pattern.getFormattedValue();
        if (ImGui::InputText("##Value", value, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
            wolv::math_eval::MathEvaluator<i128> mathEvaluator;

            if (auto result = mathEvaluator.evaluate(value); result.has_value())
                pattern.setValue(result.value());

            this->resetEditing();
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternString& pattern) {
        if (pattern.getSize() > 0) {
            createDefaultEntry(pattern);

            if (!this->isEditingPattern(pattern)) {
                drawValueColumn(pattern);
                return;
            }
            
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

            auto value = pattern.toString();
            if (ImGui::InputText("##Value", value.data(), value.size() + 1, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
                pattern.setValue(value);
                this->resetEditing();
            }

            ImGui::PopItemWidth();
            ImGui::PopStyleVar();
        }
    }

    void PatternDrawer::visit(pl::ptrn::PatternStruct& pattern) {
        bool open = true;

        if (!pattern.isInlined() && m_treeStyle != TreeStyle::Flattened) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            open = createTreeNode(pattern);
            ImGui::SameLine(0, 0);
            makeSelectable(pattern);
            drawCommentTooltip(pattern);
            ImGui::TableNextColumn();
            if (pattern.isSealed())
                drawColorColumn(pattern);
            else
                ImGui::TableNextColumn();
            drawOffsetColumn(pattern);
            drawSizeColumn(pattern);
            drawTypenameColumn(pattern, "struct");

            if (this->isEditingPattern(pattern) && !pattern.getWriteFormatterFunction().empty()) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                auto value = pattern.toString();
                if (ImGui::InputText("##Value", value, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
                    pattern.setValue(value);
                    this->resetEditing();
                }
                ImGui::PopItemWidth();
                ImGui::PopStyleVar();
            } else {
                drawValueColumn(pattern);
            }

        }

        if (!open) {
            return;
        }

        int id = 1;
        pattern.forEachEntry(0, pattern.getEntryCount(), [&](u64, auto *member){
            ImGui::PushID(id);
            this->draw(*member);
            ImGui::PopID();
            id += 1;
        });

        closeTreeNode(pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternUnion& pattern) {
        bool open = true;

        if (!pattern.isInlined() && m_treeStyle != TreeStyle::Flattened) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            open = createTreeNode(pattern);
            ImGui::SameLine(0, 0);
            makeSelectable(pattern);
            drawCommentTooltip(pattern);
            ImGui::TableNextColumn();
            if (pattern.isSealed())
                drawColorColumn(pattern);
            else
                ImGui::TableNextColumn();
            drawOffsetColumn(pattern);
            drawSizeColumn(pattern);
            drawTypenameColumn(pattern, "union");

            if (this->isEditingPattern(pattern) && !pattern.getWriteFormatterFunction().empty()) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                auto value = pattern.toString();
                if (ImGui::InputText("##Value", value, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
                    pattern.setValue(value);
                    this->resetEditing();
                }
                ImGui::PopItemWidth();
                ImGui::PopStyleVar();
            } else {
                drawValueColumn(pattern);
            }
        }

        if (!open) {
            return;
        }

        int id = 1;
        pattern.forEachEntry(0, pattern.getEntryCount(), [&](u64, auto *member) {
            ImGui::PushID(id);
            this->draw(*member);
            ImGui::PopID();

            id += 1;
        });

        closeTreeNode(pattern.isInlined());
    }

    void PatternDrawer::visit(pl::ptrn::PatternUnsigned& pattern) {
        createDefaultEntry(pattern);

        if (!this->isEditingPattern(pattern)) {
            drawValueColumn(pattern);
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        auto value = pattern.toString();
        if (ImGui::InputText("##Value", value, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue)) {
            wolv::math_eval::MathEvaluator<u128> mathEvaluator;

            if (auto result = mathEvaluator.evaluate(value); result.has_value())
                pattern.setValue(result.value());

            this->resetEditing();
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
    }

    void PatternDrawer::visit(pl::ptrn::PatternWideCharacter& pattern) {
        createDefaultEntry(pattern);
        drawValueColumn(pattern);
    }

    void PatternDrawer::visit(pl::ptrn::PatternWideString& pattern) {
        if (pattern.getSize() > 0) {
            createDefaultEntry(pattern);
            drawValueColumn(pattern);
        }
    }

    void PatternDrawer::draw(pl::ptrn::Pattern& pattern) {
        if (pattern.getVisibility() == pl::ptrn::Visibility::Hidden)
            return;

        m_currPatternPath.push_back(pattern.getVariableName());
        ON_SCOPE_EXIT { m_currPatternPath.pop_back(); };

        if (matchesFilter(m_filter.path, m_currPatternPath, false)) {
            if (m_filter.value.has_value()) {
                auto patternValue = pattern.getValue();
                if (patternValue == m_filter.value)
                    pattern.accept(*this);
                else if (!matchesFilter(m_filter.path, m_currPatternPath, true))
                    pattern.accept(*this);
                else if (patternValue.isPattern() && m_filter.value->isString()) {
                    if (patternValue.toString(true) == m_filter.value->toString(false))
                        pattern.accept(*this);
                }
            } else {
                pattern.accept(*this);
            }
        }
    }

    void PatternDrawer::drawArray(pl::ptrn::Pattern& pattern, pl::ptrn::IIterable &iterable, bool isInlined) {
        if (iterable.getEntryCount() == 0)
            return;

        bool open = true;
        if (!isInlined && m_treeStyle != TreeStyle::Flattened) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            open = createTreeNode(pattern);
            ImGui::SameLine(0, 0);
            makeSelectable(pattern);
            drawCommentTooltip(pattern);
            ImGui::TableNextColumn();

            if (pattern.isSealed())
                drawColorColumn(pattern);
            else
                ImGui::TableNextColumn();
            drawOffsetColumn(pattern);
            drawSizeColumn(pattern);
            ImGuiExt::TextFormattedColored(ImColor(0xFF9BC64D), "{0}", pattern.getTypeName());
            ImGui::SameLine(0, 0);

            ImGui::TextUnformatted("[");
            ImGui::SameLine(0, 0);
            ImGuiExt::TextFormattedColored(ImColor(0xFF00FF00), "{0}", iterable.getEntryCount());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted("]");

            ImGui::TableNextColumn();

            drawValueColumn(pattern);
        }

        if (!open) {
            return;
        }

        u64 chunkCount = 0;
        for (u64 i = 0; i < iterable.getEntryCount(); i += ChunkSize) {
            chunkCount++;

            auto &displayEnd = this->getDisplayEnd(pattern);
            if (chunkCount > displayEnd) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();

                ImGui::Selectable(hex::format("... ({})", "hex.ui.pattern_drawer.double_click"_lang).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    displayEnd += DisplayEndStep;
                break;
            }

            auto endIndex = std::min<u64>(iterable.getEntryCount(), i + ChunkSize);

            bool chunkOpen = true;
            if (iterable.getEntryCount() > ChunkSize) {
                auto startOffset = iterable.getEntry(i)->getOffset();
                auto endOffset = iterable.getEntry(endIndex - 1)->getOffset();
                auto endSize = iterable.getEntry(endIndex - 1)->getSize();

                size_t chunkSize = (endOffset - startOffset) + endSize;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();

                chunkOpen = highlightWhenSelected(startOffset, ((endOffset + endSize) - startOffset) - 1, [&]{
                    return ImGui::TreeNodeEx(hex::format("{0}[{1} ... {2}]", m_treeStyle == TreeStyle::Flattened ? this->getDisplayName(pattern).c_str() : "", i, endIndex - 1).c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                });

                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                drawOffsetColumn(pattern);
                ImGuiExt::TextFormatted("0x{0:04X}", chunkSize);
                ImGui::TableNextColumn();
                ImGuiExt::TextFormattedColored(ImColor(0xFF9BC64D), "{0}", pattern.getTypeName());
                ImGui::SameLine(0, 0);

                ImGui::TextUnformatted("[");
                ImGui::SameLine(0, 0);
                ImGuiExt::TextFormattedColored(ImColor(0xFF00FF00), "{0}", endIndex - i);
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted("]");

                ImGui::TableNextColumn();
                ImGuiExt::TextFormatted("[ ... ]");
            }


            if (!chunkOpen) {
                continue;
            }
            
            int id = 1;
            iterable.forEachEntry(i, endIndex, [&](u64, auto *entry){
                ImGui::PushID(id);
                this->draw(*entry);
                ImGui::PopID();

                id += 1;
            });

            if (iterable.getEntryCount() > ChunkSize)
                ImGui::TreePop();
        }

        closeTreeNode(isInlined);
    }

    u64& PatternDrawer::getDisplayEnd(const pl::ptrn::Pattern& pattern) {
        auto it = m_displayEnd.find(&pattern);
        if (it != m_displayEnd.end()) {
            return it->second;
        }

        auto [value, success] = m_displayEnd.emplace(&pattern, DisplayEndDefault);
        return value->second;
    }

    bool PatternDrawer::sortPatterns(const ImGuiTableSortSpecs* sortSpecs, const pl::ptrn::Pattern * left, const pl::ptrn::Pattern * right) const {
        if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("name")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return this->getDisplayName(*left) < this->getDisplayName(*right);
            else
                return this->getDisplayName(*left) > this->getDisplayName(*right);
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("start")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getOffsetForSorting() < right->getOffsetForSorting();
            else
                return left->getOffsetForSorting() > right->getOffsetForSorting();
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("end")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getOffsetForSorting() + left->getSize() < right->getOffsetForSorting() + right->getSize();
            else
                return left->getOffsetForSorting() + left->getSize() > right->getOffsetForSorting() + right->getSize();
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("size")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getSizeForSorting() < right->getSizeForSorting();
            else
                return left->getSizeForSorting() > right->getSizeForSorting();
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("value")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getValue().toString(true) < right->getValue().toString(true);
            else
                return left->getValue().toString(true) > right->getValue().toString(true);
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("type")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getTypeName() < right->getTypeName();
            else
                return left->getTypeName() > right->getTypeName();
        } else if (sortSpecs->Specs->ColumnUserID == ImGui::GetID("color")) {
            if (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending)
                return left->getColor() < right->getColor();
            else
                return left->getColor() > right->getColor();
        }

        return false;
    }

    bool PatternDrawer::beginPatternTable(const std::vector<std::shared_ptr<pl::ptrn::Pattern>> &patterns, std::vector<pl::ptrn::Pattern*> &sortedPatterns, float height) const {
        if (!ImGui::BeginTable("##Patterntable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, height))) {
            return false;
        }
    
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##favorite",                               ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_IndentDisable, ImGui::GetTextLineHeight(), ImGui::GetID("favorite"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.var_name"_lang, ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_IndentEnable, 0, ImGui::GetID("name"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.color"_lang,    ImGuiTableColumnFlags_PreferSortAscending, 0, ImGui::GetID("color"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.start"_lang,    ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_DefaultSort, 0, ImGui::GetID("start"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.end"_lang,      ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_DefaultSort, 0, ImGui::GetID("end"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.size"_lang,     ImGuiTableColumnFlags_PreferSortAscending, 0, ImGui::GetID("size"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.type"_lang,     ImGuiTableColumnFlags_PreferSortAscending, 0, ImGui::GetID("type"));
        ImGui::TableSetupColumn("hex.ui.pattern_drawer.value"_lang,    ImGuiTableColumnFlags_PreferSortAscending, 0, ImGui::GetID("value"));

        auto sortSpecs = ImGui::TableGetSortSpecs();

        if (patterns.empty()) {
            sortedPatterns.clear();
            return true;
        }

        if (!sortSpecs->SpecsDirty && !sortedPatterns.empty()) {
            return true;
        }
        
        sortedPatterns.clear();
        std::transform(patterns.begin(), patterns.end(), std::back_inserter(sortedPatterns), [](const std::shared_ptr<pl::ptrn::Pattern> &pattern) {
            return pattern.get();
        });

        std::sort(sortedPatterns.begin(), sortedPatterns.end(), [this, &sortSpecs](const pl::ptrn::Pattern *left, const pl::ptrn::Pattern *right) -> bool {
            return this->sortPatterns(sortSpecs, left, right);
        });

        for (auto &pattern : sortedPatterns)
            pattern->sort([this, &sortSpecs](const pl::ptrn::Pattern *left, const pl::ptrn::Pattern *right){
                return this->sortPatterns(sortSpecs, left, right);
            });

        sortSpecs->SpecsDirty = false;
        
        return true;
    }

    void PatternDrawer::traversePatternTree(pl::ptrn::Pattern &pattern, std::vector<std::string> &patternPath, const std::function<void(pl::ptrn::Pattern&)> &callback) {
        patternPath.push_back(pattern.getVariableName());
        ON_SCOPE_EXIT { patternPath.pop_back(); };

        callback(pattern);
        if (auto iterable = dynamic_cast<pl::ptrn::IIterable*>(&pattern); iterable != nullptr) {
            iterable->forEachEntry(0, iterable->getEntryCount(), [&](u64, pl::ptrn::Pattern *entry) {
                traversePatternTree(*entry, patternPath, callback);
            });
        }
    }

    void PatternDrawer::draw(const std::vector<std::shared_ptr<pl::ptrn::Pattern>> &patterns, pl::PatternLanguage *runtime, float height) {
        std::scoped_lock lock(s_resetDrawMutex);

        const auto treeStyleButton = [this](auto icon, TreeStyle style, const char *tooltip) {
            bool pushed = false;

            if (m_treeStyle == style) {
                ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                pushed = true;
            }

            if (ImGuiExt::DimmedIconButton(icon, ImGui::GetStyleColorVec4(ImGuiCol_Text)))
                m_treeStyle = style;

            if (pushed)
                ImGui::PopStyleColor();

            ImGuiExt::InfoTooltip(tooltip);
        };

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
            this->resetEditing();
        }

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetTextLineHeightWithSpacing() * 9.5);
        if (ImGuiExt::InputTextIcon("##Search", ICON_VS_FILTER, m_filterText)) {
            m_filter = parseRValueFilter(m_filterText).value_or(Filter{ });
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();

        ImGuiExt::DimmedIconToggle(ICON_VS_BOOK, &m_showSpecName);
        ImGuiExt::InfoTooltip("hex.ui.pattern_drawer.spec_name"_lang);

        ImGui::SameLine();

        treeStyleButton(ICON_VS_SYMBOL_KEYWORD, TreeStyle::Default,         "hex.ui.pattern_drawer.tree_style.tree"_lang);
        ImGui::SameLine(0, 0);
        treeStyleButton(ICON_VS_LIST_TREE,      TreeStyle::AutoExpanded,    "hex.ui.pattern_drawer.tree_style.auto_expanded"_lang);
        ImGui::SameLine(0, 0);
        treeStyleButton(ICON_VS_LIST_FLAT,      TreeStyle::Flattened,       "hex.ui.pattern_drawer.tree_style.flattened"_lang);

        ImGui::SameLine(0, 15_scaled);

        const auto startPos = ImGui::GetCursorPos();

        ImGui::BeginDisabled(runtime == nullptr);
        if (ImGuiExt::DimmedIconButton(ICON_VS_EXPORT, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::OpenPopup("ExportPatterns");
        }
        ImGui::EndDisabled();

        ImGuiExt::InfoTooltip("hex.ui.pattern_drawer.export"_lang);

        ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImVec2(startPos.x, ImGui::GetCursorPosY()));
        if (ImGui::BeginPopup("ExportPatterns")) {
            for (const auto &formatter : m_formatters) {
                const auto name = [&]{
                    auto name = formatter->getName();
                    std::transform(name.begin(), name.end(), name.begin(), [](char c){ return char(std::toupper(c)); });

                    return name;
                }();

                const auto &extension = formatter->getFileExtension();

                if (ImGui::MenuItem(name.c_str())) {
                    fs::openFileBrowser(fs::DialogMode::Save, { { name.c_str(), extension.c_str() } }, [&](const std::fs::path &path) {
                        auto result = formatter->format(*runtime);

                        wolv::io::File output(path, wolv::io::File::Mode::Create);
                        output.writeVector(result);
                    });
                }
            }
            ImGui::EndPopup();
        }

        if (!m_favoritesUpdated) {
            m_favoritesUpdated = true;

            if (!patterns.empty() && !m_favoritesUpdateTask.isRunning()) {
                m_favoritesUpdateTask = TaskManager::createTask("hex.ui.pattern_drawer.updating"_lang, TaskManager::NoProgress, [this, patterns](auto &task) {
                    size_t updatedFavorites = 0;

                    for (auto &pattern : patterns) {
                        std::vector<std::string> patternPath;
                        traversePatternTree(*pattern, patternPath, [&, this](const pl::ptrn::Pattern &pattern) {
                            if (pattern.hasAttribute("hex::favorite"))
                                m_favorites.insert({ patternPath, pattern.clone() });

                            if (const auto &args = pattern.getAttributeArguments("hex::group"); !args.empty()) {
                                auto groupName = args.front().toString();

                                if (!m_groups.contains(groupName))
                                    m_groups.insert({groupName, std::vector<std::unique_ptr<pl::ptrn::Pattern>>()});

                                m_groups[groupName].push_back(pattern.clone());
                            }
                        });

                        if (updatedFavorites == m_favorites.size())
                            task.interrupt();
                        task.update();

                        patternPath.clear();
                        traversePatternTree(*pattern, patternPath, [&, this](const pl::ptrn::Pattern &pattern) {
                            for (auto &[path, favoritePattern] : m_favorites) {
                                if (updatedFavorites == m_favorites.size())
                                    task.interrupt();
                                task.update();

                                if (this->matchesFilter(patternPath, path, true)) {
                                    favoritePattern = pattern.clone();
                                    updatedFavorites += 1;

                                    break;
                                }
                            }
                        });
                    }

                    std::erase_if(m_favorites, [](const auto &entry) {
                        const auto &[path, favoritePattern] = entry;

                        return favoritePattern == nullptr;
                    });
                });
            }

        }

        if (beginPatternTable(patterns, m_sortedPatterns, height)) {
            ImGui::TableHeadersRow();

            m_showFavoriteStars = false;
            if (!m_favoritesUpdateTask.isRunning()) {
                int id = 1;
                bool doTableNextRow = false;

                if (!m_favorites.empty() && !patterns.empty()) {
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::PushID(id);
                    if (ImGui::TreeNodeEx("hex.ui.pattern_drawer.favorites"_lang, ImGuiTreeNodeFlags_SpanFullWidth)) {
                        for (auto &[path, pattern] : m_favorites) {
                            if (pattern == nullptr)
                                continue;

                            ImGui::PushID(pattern->getDisplayName().c_str());
                            this->draw(*pattern);
                            ImGui::PopID();
                        }

                        ImGui::TreePop();
                    }
                    ImGui::PopID();

                    id += 1;
                    doTableNextRow = true;
                }

                if (!m_groups.empty() && !patterns.empty()) {
                    for (auto &[groupName, groupPatterns]: m_groups) {
                        if (doTableNextRow) {
                            ImGui::TableNextRow();
                        }

                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::PushID(id);
                        if (ImGui::TreeNodeEx(groupName.c_str(), ImGuiTreeNodeFlags_SpanFullWidth)) {
                            for (auto &groupPattern: groupPatterns) {
                                if (groupPattern == nullptr)
                                    continue;

                                ImGui::PushID(id);
                                this->draw(*groupPattern);
                                ImGui::PopID();

                                id += 1;
                            }

                            ImGui::TreePop();
                        }
                        ImGui::PopID();

                        id += 1;
                        doTableNextRow = true;
                    }
                }

                m_showFavoriteStars = true;

                for (auto &pattern : m_sortedPatterns) {
                    ImGui::PushID(id);
                    this->draw(*pattern);
                    ImGui::PopID();

                    id += 1;
                }
            }

            ImGui::EndTable();
        }

        if (m_favoritesUpdateTask.isRunning()) {
            ImGuiExt::TextOverlay("hex.ui.pattern_drawer.updating"_lang, ImGui::GetWindowPos() + ImGui::GetWindowSize() / 2);
        }
    }

    void PatternDrawer::reset() {
        std::scoped_lock lock(s_resetDrawMutex);

        this->resetEditing();
        m_displayEnd.clear();
        m_visualizedPatterns.clear();
        m_currVisualizedPattern = nullptr;
        m_sortedPatterns.clear();
        m_lastVisualizerError.clear();
        m_currPatternPath.clear();

        m_favoritesUpdateTask.interrupt();

        for (auto &[path, pattern] : m_favorites)
            pattern = nullptr;
        for (auto &[groupName, patterns]: m_groups)
            for (auto &pattern: patterns)
                pattern = nullptr;

        m_groups.clear();

        m_favoritesUpdated = false;
    }
}