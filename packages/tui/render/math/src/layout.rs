use term_maths::RenderedBlock as Block;

pub(crate) fn text(value: &str) -> Block {
    Block::from_text(value)
}

fn grouped(value: &str) -> String {
    if value.chars().all(|ch| ch.is_alphanumeric() || ch == '′') {
        value.to_owned()
    } else {
        format!("({value})")
    }
}

pub(crate) fn fraction(top: Block, bottom: Block, rule: bool, display: bool) -> Option<Block> {
    if !display {
        if !rule {
            return None;
        }
        let (top, bottom) = (top.to_string(), bottom.to_string());
        let (top, bottom) = (top.trim(), bottom.trim());
        let common = match (top, bottom) {
            ("1", "2") => "½",
            ("1", "3") => "⅓",
            ("2", "3") => "⅔",
            ("1", "4") => "¼",
            ("3", "4") => "¾",
            _ => return Some(text(&format!("{}⁄{}", grouped(top), grouped(bottom)))),
        };
        return Some(text(common));
    }
    let width = top.width().max(bottom.width()).max(1);
    let baseline = top.height();
    let bar = Block::hline(if rule { '─' } else { ' ' }, width);
    let top = Block::above(&top.center_in(width), &bar, baseline);
    Some(Block::above(&top, &bottom.center_in(width), baseline))
}

fn script_glyphs(value: &str, above: bool) -> Option<String> {
    let (source, glyphs) = if above {
        (
            "0123456789+-=()abcdefghijklmnoprstuvwxyz",
            "⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ᵃᵇᶜᵈᵉᶠᵍʰⁱʲᵏˡᵐⁿᵒᵖʳˢᵗᵘᵛʷˣʸᶻ",
        )
    } else {
        (
            "0123456789+-=()aehijklmnoprstuvxβγρφχ",
            "₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎ₐₑₕᵢⱼₖₗₘₙₒₚᵣₛₜᵤᵥₓᵦᵧᵨᵩᵪ",
        )
    };
    value
        .chars()
        .map(|ch| {
            let ch = if ch == '−' { '-' } else { ch };
            source
                .chars()
                .position(|c| c == ch)
                .and_then(|i| glyphs.chars().nth(i))
        })
        .collect()
}

fn combining_accent(value: &str, above: bool) -> Option<char> {
    Some(match (value, above) {
        ("‾", true) => '\u{0305}',
        ("^", true) => '\u{0302}',
        ("~", true) => '\u{0303}',
        ("→", true) => '\u{20d7}',
        ("←", true) => '\u{20d6}',
        ("˙", true) => '\u{0307}',
        ("¨", true) => '\u{0308}',
        ("˘", true) => '\u{0306}',
        ("ˇ", true) => '\u{030c}',
        ("\u{0060}", true) => '\u{0300}',
        ("˚", true) => '\u{030a}',
        ("_", false) => '\u{0332}',
        _ => return None,
    })
}

pub(crate) fn scripts(
    base: Block,
    lower: Option<Block>,
    upper: Option<Block>,
    stacked: bool,
    display: bool,
) -> Option<Block> {
    if stacked && (lower.is_none() != upper.is_none()) {
        let above = upper.is_some();
        let mark = upper.as_ref().or(lower.as_ref())?.to_string();
        if let Some(accent) = combining_accent(mark.trim(), above) {
            let source = base.to_string();
            if source.chars().count() == 1 {
                return Some(text(&format!("{source}{accent}")));
            }
            if display && matches!(accent, '\u{0305}' | '\u{0332}') {
                let line = Block::hline('─', base.width());
                return Some(if above {
                    Block::above(&line, &base, line.height() + base.baseline())
                } else {
                    Block::above(&base, &line, base.baseline())
                });
            }
        }
    }
    if stacked && !display {
        return None;
    }
    if !display || !stacked && (lower.is_none() || upper.is_none()) {
        let mut compact = base.clone();
        let mut fits = true;
        for (mark, value) in [('_', lower.as_ref()), ('^', upper.as_ref())] {
            if let Some(value) = value {
                let value = value.to_string();
                let value = value.trim();
                if let Some(mapped) = script_glyphs(value, mark == '^')
                    .filter(|glyphs| !display || glyphs.chars().count() == 1)
                {
                    compact = compact.beside(&text(&mapped));
                } else if !display {
                    compact = compact.beside(&text(&format!("{mark}{}", grouped(value))));
                } else {
                    fits = false;
                    break;
                }
            }
        }
        if fits {
            return Some(compact);
        }
    }
    let width = lower
        .as_ref()
        .map_or(0, Block::width)
        .max(upper.as_ref().map_or(0, Block::width));
    let middle = if stacked {
        base.center_in(width)
    } else {
        Block::hline(' ', width)
    };
    let width = middle.width();
    let pad = |block: Block| {
        if stacked {
            block.center_in(width)
        } else {
            block.pad(0, width - block.width(), 0, 0)
        }
    };
    let middle = match upper {
        Some(upper) => {
            let baseline = upper.height() + middle.baseline();
            Block::above(&pad(upper), &middle, baseline)
        }
        None => middle,
    };
    let block = match lower {
        Some(lower) => Block::above(&middle, &pad(lower), middle.baseline()),
        None => middle,
    };
    Some(if stacked { block } else { base.beside(&block) })
}

pub(crate) fn root(body: Block, index: Option<Block>, display: bool) -> Block {
    let index = index.map_or_else(String::new, |value| {
        let value = value.to_string();
        let value = value.trim();
        script_glyphs(value, true).unwrap_or_else(|| format!("[{value}]"))
    });
    let mark = text(&format!("{index}√"));
    if !display {
        return mark.beside(&text(&grouped(body.to_string().trim())));
    }
    let baseline = body.baseline() + 1;
    let bar = Block::hline('─', body.width());
    mark.beside(&Block::above(&bar, &body, baseline))
}

pub(crate) fn fenced(body: Block, left: Option<char>, right: Option<char>) -> Block {
    fn fence(ch: char, height: usize, baseline: usize) -> Block {
        let pieces = match ch {
            '(' => ['⎛', '⎜', '⎜', '⎝'],
            ')' => ['⎞', '⎟', '⎟', '⎠'],
            '[' => ['⎡', '⎢', '⎢', '⎣'],
            ']' => ['⎤', '⎥', '⎥', '⎦'],
            '{' => ['⎧', '⎪', '⎨', '⎩'],
            '}' => ['⎫', '⎪', '⎬', '⎭'],
            ch => [ch; 4],
        };
        if height <= 1 {
            return Block::from_char(ch);
        }
        let rows = (0..height)
            .map(|i| {
                let part = if i == 0 {
                    0
                } else if i + 1 == height {
                    3
                } else if i == baseline {
                    2
                } else {
                    1
                };
                vec![pieces[part].to_string()]
            })
            .collect();
        Block::new(rows, baseline)
    }
    let (height, baseline) = (body.height(), body.baseline());
    let body = match left {
        Some(c) => fence(c, height, baseline).beside(&body),
        None => body,
    };
    match right {
        Some(c) => body.beside(&fence(c, height, baseline)),
        None => body,
    }
}
