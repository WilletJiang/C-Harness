use pulldown_latex::event::{
    ColumnAlignment, Content, EnvironmentFlow, Event, Font, Grouping, ScriptPosition, ScriptType,
    StateChange, Visual,
};
use pulldown_latex::{Parser, Storage};
use rust_latex_parser::MathFontKind;
use std::iter::Peekable;
use std::slice::Iter;
use term_maths::mathfont;
use term_maths::RenderedBlock as Block;
use unicode_width::UnicodeWidthStr;

mod tui_math_layout;
mod tui_math_png;
use tui_math_layout::{fenced, fraction, root, scripts, text};

type Events<'a> = Peekable<Iter<'a, Event<'a>>>;

#[derive(Clone, Copy)]
struct Layout {
    display: bool,
    font: Option<Font>,
}

impl Layout {
    fn inline(self) -> Self {
        Self {
            display: false,
            ..self
        }
    }

    fn glyphs(self, value: &str) -> Block {
        let kind = match self.font {
            Some(Font::DoubleStruck) => MathFontKind::Blackboard,
            Some(Font::Script | Font::BoldScript) => MathFontKind::Calligraphic,
            Some(Font::Fraktur | Font::BoldFraktur) => MathFontKind::Fraktur,
            Some(Font::Bold | Font::BoldItalic) => MathFontKind::Bold,
            Some(
                Font::SansSerif
                | Font::SansSerifItalic
                | Font::BoldSansSerif
                | Font::SansSerifBoldItalic,
            ) => MathFontKind::SansSerif,
            Some(Font::Monospace) => MathFontKind::Monospace,
            _ => return text(value),
        };
        text(&mathfont::map_str(&kind, value))
    }
}

fn content(value: &Content<'_>, layout: Layout) -> Block {
    match value {
        Content::Text(s) => text(if *s == "&nbsp;" { " " } else { s }),
        Content::Function(s) => text(s),
        Content::Number(s) => layout.glyphs(s),
        Content::Ordinary { content, .. } => layout.glyphs(content.encode_utf8(&mut [0; 4])),
        Content::LargeOp { content, .. }
        | Content::Delimiter { content, .. }
        | Content::Punctuation(content) => Block::from_char(*content),
        Content::BinaryOp { content, .. } => text(&format!(" {content} ")),
        Content::Relation { content, .. } => {
            let mut buffer = [0; 8];
            let relation = content.encode_utf8_to_buf(&mut buffer);
            let relation = std::str::from_utf8(relation).expect("encoded Unicode relation");
            text(&format!(" {relation} "))
        }
    }
}

fn sequence(input: &mut Events<'_>, mut layout: Layout, depth: usize) -> Option<Block> {
    let mut result = Block::empty();
    let mut prefix = true;
    while let Some(&event) = input.peek() {
        if matches!(event, Event::End | Event::EnvironmentFlow(_)) {
            break;
        }
        if let Event::StateChange(state) = event {
            match state {
                StateChange::Font(font) => layout.font = *font,
                StateChange::Style(_) => {}
                StateChange::Color(_) => return None,
            }
            input.next();
            continue;
        }
        let atom = match event {
            Event::Content(Content::BinaryOp {
                content: ch @ ('+' | '-' | '−' | '±' | '∓'),
                ..
            }) if prefix => {
                let sign = Block::from_char(*ch);
                input.next();
                sign
            }
            _ => element(input, layout, depth)?,
        };
        prefix = match event {
            Event::Content(Content::Delimiter { content, .. }) => "([{⟨".contains(*content),
            Event::Space { .. } => prefix,
            _ => matches!(
                event,
                Event::Content(Content::BinaryOp { .. } | Content::Relation { .. })
            ),
        };
        result = result.beside(&atom);
        if result.width() > 256 || result.height() > 48 {
            return None;
        }
    }
    Some(result)
}

enum Align {
    Left,
    Center,
    Right,
    Pairs,
}

fn grid(input: &mut Events<'_>, layout: Layout, depth: usize, align: Align) -> Option<Block> {
    let mut rows = Vec::new();
    let mut row = Vec::new();
    loop {
        row.push(sequence(input, layout, depth)?);
        match input.next()? {
            Event::EnvironmentFlow(EnvironmentFlow::Alignment) => {}
            Event::EnvironmentFlow(EnvironmentFlow::NewLine {
                horizontal_lines,
                spacing,
            }) if horizontal_lines.is_empty() && spacing.is_none() => {
                rows.push(std::mem::take(&mut row));
                if matches!(input.peek(), Some(Event::End)) {
                    input.next();
                    break;
                }
            }
            Event::End => {
                rows.push(row);
                break;
            }
            _ => return None,
        }
    }
    let mut widths = [0; 24];
    for row in &rows {
        for (i, cell) in row.iter().enumerate() {
            let width = widths.get_mut(i)?;
            *width = (*width).max(cell.width());
        }
    }
    let mut result = Block::empty();
    for row in rows {
        let mut line = Block::empty();
        for (i, cell) in row.into_iter().enumerate() {
            let padding = widths[i] - cell.width();
            let cell = match align {
                Align::Center => cell.center_in(widths[i]),
                Align::Right => cell.pad(padding, 0, 0, 0),
                Align::Pairs if i % 2 == 0 => cell.pad(padding, 0, 0, 0),
                _ => cell.pad(0, padding, 0, 0),
            };
            if i > 0 && (!matches!(align, Align::Pairs) || i % 2 == 0) {
                line = line.beside(&text("  "));
            }
            line = line.beside(&cell);
        }
        result = if result.is_empty() {
            line
        } else {
            let baseline = (result.height() + line.height()) / 2;
            Block::above(&result, &line, baseline)
        };
    }
    Some(result)
}

fn script(
    input: &mut Events<'_>,
    ty: &ScriptType,
    position: &ScriptPosition,
    layout: Layout,
    depth: usize,
) -> Option<Block> {
    let base = element(input, layout, depth)?;
    let stacked = matches!(position, ScriptPosition::AboveBelow)
        || layout.display && matches!(position, ScriptPosition::Movable);
    let child = if stacked { layout } else { layout.inline() };
    let mut lower = None;
    let mut upper = None;
    if matches!(ty, ScriptType::Subscript | ScriptType::SubSuperscript) {
        lower = Some(element(input, child, depth)?);
    }
    if matches!(ty, ScriptType::Superscript | ScriptType::SubSuperscript) {
        upper = Some(element(input, child, depth)?);
    }
    scripts(base, lower, upper, stacked, layout.display)
}

fn group(
    input: &mut Events<'_>,
    grouping: &Grouping,
    layout: Layout,
    depth: usize,
) -> Option<Block> {
    match grouping {
        Grouping::Normal | Grouping::LeftRight(_, _) | Grouping::Equation { .. } => {
            let body = sequence(input, layout, depth)?;
            if !matches!(input.next()?, Event::End) {
                return None;
            }
            match grouping {
                Grouping::LeftRight(left, right) => Some(fenced(body, *left, *right)),
                _ => Some(body),
            }
        }
        Grouping::Matrix { alignment } if layout.display => {
            let align = match alignment {
                ColumnAlignment::Left => Align::Left,
                ColumnAlignment::Right => Align::Right,
                ColumnAlignment::Center => Align::Center,
            };
            grid(input, layout, depth, align)
        }
        Grouping::Aligned
        | Grouping::Align { .. }
        | Grouping::Alignedat { .. }
        | Grouping::Alignat { .. }
        | Grouping::Split
            if layout.display =>
        {
            grid(input, layout, depth, Align::Pairs)
        }
        Grouping::Gathered | Grouping::Gather { .. } | Grouping::Multline if layout.display => {
            grid(input, layout, depth, Align::Center)
        }
        Grouping::Cases { left } if layout.display => {
            let body = grid(input, layout, depth, Align::Left)?;
            Some(fenced(body, left.then_some('{'), (!left).then_some('}')))
        }
        _ => None,
    }
}

fn element(input: &mut Events<'_>, layout: Layout, depth: usize) -> Option<Block> {
    if depth >= 24 {
        return None;
    }
    let depth = depth + 1;
    let result = match input.next()? {
        Event::Content(value) => content(value, layout),
        Event::Begin(grouping) => group(input, grouping, layout, depth)?,
        Event::Visual(Visual::Fraction(rule)) => {
            let top = element(input, layout, depth)?;
            let bottom = element(input, layout, depth)?;
            fraction(
                top,
                bottom,
                rule.is_none_or(|r| r.value != 0.0),
                layout.display,
            )?
        }
        Event::Visual(Visual::SquareRoot) => {
            root(element(input, layout, depth)?, None, layout.display)
        }
        Event::Visual(Visual::Root) => {
            let body = element(input, layout, depth)?;
            let index = element(input, layout.inline(), depth)?;
            root(body, Some(index), layout.display)
        }
        Event::Visual(Visual::Negation) => {
            let body = element(input, layout.inline(), depth)?.to_string();
            let body = body.trim();
            if body.chars().count() != 1 {
                return None;
            }
            text(&format!("{body}\u{0338}"))
        }
        Event::Script { ty, position } => script(input, ty, position, layout, depth)?,
        Event::Space { width, .. } => {
            if width.is_some_and(|dimension| dimension.value > 0.0) {
                text(" ")
            } else {
                Block::empty()
            }
        }
        _ => return None,
    };
    (result.width() <= 256 && result.height() <= 48).then_some(result)
}

fn bounded_source(source: &str) -> bool {
    let mut chars = source.char_indices().peekable();
    let mut depth = 0usize;
    while let Some((i, ch)) = chars.next() {
        match ch {
            '{' => {
                depth += 1;
                if depth >= 24 {
                    return false;
                }
            }
            '}' => depth = depth.saturating_sub(1),
            '\\' => {
                if !chars.peek().is_some_and(|(_, c)| c.is_ascii_alphabetic()) {
                    chars.next();
                    continue;
                }
                let start = i + 1;
                let mut end = start;
                while let Some((at, _)) = chars.next_if(|(_, c)| c.is_ascii_alphabetic()) {
                    end = at + 1;
                }
                if matches!(
                    &source[start..end],
                    "def" | "let" | "futurelet" | "newcommand" | "renewcommand" | "providecommand"
                ) {
                    return false;
                }
            }
            _ => {}
        }
    }
    true
}

fn render(source: &str, display: bool, cols: usize) -> Option<String> {
    if !bounded_source(source) {
        return None;
    }
    let storage = Storage::new();
    let events = Parser::new(source, &storage)
        .take(257)
        .collect::<Result<Vec<_>, _>>()
        .ok()?;
    if events.len() > 256 {
        return None;
    }
    let mut stream = events.iter().peekable();
    let block = sequence(
        &mut stream,
        Layout {
            display,
            font: None,
        },
        0,
    )?;
    if stream.next().is_some() {
        return None;
    }
    let output = block.to_string();
    let output = if display {
        output
            .lines()
            .map(str::trim_end)
            .collect::<Vec<_>>()
            .join("\n")
    } else {
        output.trim().to_owned()
    };
    if !display && output.contains(['\n', '\r', '\t']) {
        return None;
    }
    (output.lines().all(|line| line.width() <= cols)).then_some(output)
}

// Both buffers belong to the caller. A negative return requests original source.
#[no_mangle]
pub unsafe extern "C" fn ah_tui_math(
    source: *const u8,
    size: usize,
    display: bool,
    cols: u32,
    out: *mut u8,
    capacity: usize,
) -> isize {
    if source.is_null() || out.is_null() || size > 4096 {
        return -1;
    }
    let Ok(source) = std::str::from_utf8(std::slice::from_raw_parts(source, size)) else {
        return -1;
    };
    let Ok(Some(result)) = std::panic::catch_unwind(|| render(source, display, cols as usize))
    else {
        return -1;
    };
    if result.len() > capacity {
        return -1;
    }
    std::ptr::copy_nonoverlapping(result.as_ptr(), out, result.len());
    result.len() as isize
}

#[cfg(test)]
mod tests {
    use super::render;

    #[test]
    fn inline_scripts_fractions_and_symbols() {
        assert_eq!(
            render(r"x_i^2+\frac{1}{2} \in \mathbb{R}", false, 80).unwrap(),
            "xᵢ² + ½ ∈ ℝ"
        );
        assert_eq!(
            render(r"\hat{x}+\sqrt[3]{8}", false, 80).unwrap(),
            "x̂ + ³√8"
        );
    }

    #[test]
    fn display_layout_keeps_the_baseline_and_alignment() {
        assert_eq!(
            render(r"y = \frac{a+b+c}{3}", true, 80).unwrap(),
            "    a + b + c\ny = ─────────\n        3"
        );
        let equation = render(r"\begin{aligned}a&=b+c\\long&=d\end{aligned}", true, 80).unwrap();
        let relations = equation
            .lines()
            .map(|line| line.find('=').unwrap())
            .collect::<Vec<_>>();
        assert_eq!(relations, [5, 5]);
        let root = render(r"\sqrt{x+1}", true, 80).unwrap();
        assert_eq!(root, " ─────\n√x + 1");
    }

    #[test]
    fn unsupported_and_oversized_math_is_explicit() {
        assert!(render(r"\unknown{x}", false, 80).is_none());
        assert!(render(r"\frac{x}{", true, 80).is_none());
        assert!(render(r"\newcommand{\xx}{\xx}\xx", true, 80).is_none());
        assert!(render(r"\frac{abc+def}{3}", true, 6).is_none());
    }

    #[test]
    fn characteristic_fourier_and_stokes_examples() {
        let characteristic = render(
            r"\varphi_X(t)=\mathbb{E}[e^{itX}]=\int_{-\infty}^{\infty} e^{itx} f_X(x)\,\mathrm{d}x",
            true,
            95,
        )
        .unwrap();
        assert!(characteristic.contains("𝔼") && characteristic.contains("−∞"));
        let fourier = render(
            r"\widehat{f}(\xi)=\int_{-\infty}^{\infty}f(x)e^{-2\pi i x\xi}\,\mathrm{d}x",
            true,
            95,
        )
        .unwrap();
        assert!(fourier.contains("f̂(ξ) = ∫") && fourier.contains("−2πixξ"));
        let stokes = render(
            r"\oint_{\partial S}\mathbf{F}\cdot\mathrm{d}\mathbf{r}=\iint_S(\nabla\times\mathbf{F})\cdot\mathrm{d}\mathbf{S}",
            true, 95,
        ).unwrap();
        assert!(stokes.contains("∮") && stokes.contains("∂S") && stokes.contains("∬"));
    }
}
