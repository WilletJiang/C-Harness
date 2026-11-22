use latex_rust::{
    layout_with_em_size_pt, parse, render_png, Color, Dim, MathFont, MathStyle, PngOptions,
};
use std::sync::OnceLock;

fn math_png(source: &str, em: u32, width: u32, height: u32, rgb: u32) -> Option<Vec<u8>> {
    if !(8..=96).contains(&em) || !super::bounded_source(source) {
        return None;
    }
    static FONT: OnceLock<Option<MathFont>> = OnceLock::new();
    let font = FONT
        .get_or_init(|| MathFont::stix_two_math().ok())
        .as_ref()?;
    let ast = parse(source).ok()?;
    let mut options = PngOptions::new();
    options.display = true;
    options.font_size_pt = Dim::from_i64(i64::from(em));
    options.dpi = Dim::from_i64(72);
    options.color = Color::rgb((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8);
    let tree =
        layout_with_em_size_pt(&ast, font, MathStyle::Display, &options.font_size_pt).ok()?;
    let scale = &options.font_size_pt;
    if &tree.width * scale > Dim::from_i64(i64::from(width.min(2048)))
        || &(&tree.height + &tree.depth) * scale > Dim::from_i64(i64::from(height.min(768)))
    {
        return None;
    }
    render_png(&tree, font, &options).ok()
}

// Caller owns both buffers. Failure selects the existing text math renderer.
#[no_mangle]
pub unsafe extern "C" fn ah_tui_math_png(
    source: *const u8,
    size: usize,
    em: u32,
    width: u32,
    height: u32,
    rgb: u32,
    output: *mut u8,
    capacity: usize,
) -> usize {
    if source.is_null() || output.is_null() || size == 0 || size > 4096 || width == 0 || height == 0
    {
        return 0;
    }
    let Ok(source) = std::str::from_utf8(std::slice::from_raw_parts(source, size)) else {
        return 0;
    };
    let Ok(Some(png)) = std::panic::catch_unwind(|| math_png(source, em, width, height, rgb))
    else {
        return 0;
    };
    if png.len() > capacity {
        return 0;
    }
    std::ptr::copy_nonoverlapping(png.as_ptr(), output, png.len());
    png.len()
}

#[cfg(test)]
mod tests {
    use super::math_png;

    #[test]
    fn transparent_display_math_respects_pixel_bounds() {
        let tex = r"\widehat{f}(\xi)=\int_{-\infty}^{\infty} f(x)e^{-2\pi i x\xi}\,\mathrm{d}x";
        let light = math_png(tex, 24, 800, 180, 0x202328).unwrap();
        let dark = math_png(tex, 24, 800, 180, 0xe8ecf4).unwrap();
        assert_eq!(&light[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(&light[16..26], &dark[16..26]);
        assert_eq!(light[25], 6); // RGBA
        assert_ne!(light, dark);
        assert!(math_png(tex, 24, 20, 180, 0x202328).is_none());
        assert!(math_png(r"\unknowncommand{x}", 24, 800, 180, 0x202328).is_none());
    }
}
