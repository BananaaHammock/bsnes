uint PPU::Line::start = 0;
uint PPU::Line::count = 0;

auto PPU::Line::flush() -> void {
  if(Line::count) {
    #pragma omp parallel for if(Line::count >= 8)
    for(uint y = 0; y < Line::count; y++) {
      ppu.lines[Line::start + y].render();
    }
    Line::start = 0;
    Line::count = 0;
  }
}

auto PPU::Line::render() -> void {
  uint y = this->y + (!ppu.latch.overscan ? 7 : 0);

  auto hd = ppu.hd();
  auto ss = ppu.ss();
  auto scale = ppu.hdScale();
  auto output = ppu.output + (!hd
  ? (y * 1024 + (ppu.interlace() && ppu.field() ? 512 : 0))
  : (y * 256 * scale * scale)
  );
  auto width = (!hd
  ? (!ppu.hires() ? 256 : 512)
  : (256 * scale * scale));

  if(io.displayDisable) {
    memory::fill<uint16>(output, width);
    return;
  }

  bool hires = io.pseudoHires || io.bgMode == 5 || io.bgMode == 6;
  auto aboveColor = cgram[0];
  auto belowColor = hires ? cgram[0] : (uint16_t)io.col.fixedColor;
  uint xa =  (hd || ss) && ppu.interlace() && ppu.field() ? 256 * scale * scale / 2 : 0;
  uint xb = !(hd || ss) ? 256 : ppu.interlace() && !ppu.field() ? 256 * scale * scale / 2 : 256 * scale * scale;
  for(uint x = xa; x < xb; x++) {
    above[x] = {Source::COL, 0, aboveColor};
    below[x] = {Source::COL, 0, belowColor};
  }

  renderBackground(io.bg1, Source::BG1);
  renderBackground(io.bg2, Source::BG2);
  renderBackground(io.bg3, Source::BG3);
  renderBackground(io.bg4, Source::BG4);
  renderObject(io.obj);
  renderWindow(io.col.window, io.col.window.aboveMask, windowAbove);
  renderWindow(io.col.window, io.col.window.belowMask, windowBelow);

  auto luma = ppu.lightTable[io.displayBrightness];
  uint curr = 0, prev = 0;
  if(hd) for(uint x : range(256 * scale * scale)) {
    *output++ = luma[pixel(x / scale & 255, above[x], below[x])];
  } else if(width == 256) for(uint x : range(256)) {
    *output++ = luma[pixel(x, above[x], below[x])];
  } else if(!hires) for(uint x : range(256)) {
    auto color = luma[pixel(x, above[x], below[x])];
    *output++ = color;
    *output++ = color;
  } else if(!configuration.video.blurEmulation) for(uint x : range(256)) {
    *output++ = luma[pixel(x, below[x], above[x])];
    *output++ = luma[pixel(x, above[x], below[x])];
  } else for(uint x : range(256)) {
    curr = luma[pixel(x, below[x], above[x])];
    *output++ = (prev + curr - ((prev ^ curr) & 0x0421)) >> 1;
    prev = curr;
    curr = luma[pixel(x, above[x], below[x])];
    *output++ = (prev + curr - ((prev ^ curr) & 0x0421)) >> 1;
    prev = curr;
  }
}

auto PPU::Line::pixel(uint x, Pixel above, Pixel below) const -> uint16_t {
  if(!windowAbove[x]) above.color = 0x0000;
  if(!windowBelow[x]) return above.color;
  if(!io.col.enable[above.source]) return above.color;
  if(!io.col.blendMode) return blend(above.color, io.col.fixedColor, io.col.halve && windowAbove[x]);
  return blend(above.color, below.color, io.col.halve && windowAbove[x] && below.source != Source::COL);
}

auto PPU::Line::blend(uint x, uint y, bool halve) const -> uint16_t {
  if(!io.col.mathMode) {  //add
    if(!halve) {
      uint sum = x + y;
      uint carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
      return (sum - carry) | (carry - (carry >> 5));
    } else {
      return (x + y - ((x ^ y) & 0x0421)) >> 1;
    }
  } else {  //sub
    uint diff = x - y + 0x8420;
    uint borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
    if(!halve) {
      return   (diff - borrow) & (borrow - (borrow >> 5));
    } else {
      return (((diff - borrow) & (borrow - (borrow >> 5))) & 0x7bde) >> 1;
    }
  }
}

auto PPU::Line::directColor(uint paletteIndex, uint paletteColor) const -> uint16_t {
  //paletteIndex = bgr
  //paletteColor = BBGGGRRR
  //output       = 0 BBb00 GGGg0 RRRr0
  return (paletteColor << 2 & 0x001c) + (paletteIndex <<  1 & 0x0002)   //R
       + (paletteColor << 4 & 0x0380) + (paletteIndex <<  5 & 0x0040)   //G
       + (paletteColor << 7 & 0x6000) + (paletteIndex << 10 & 0x1000);  //B
}

auto PPU::Line::plotAbove(uint x, uint source, uint priority, uint color) -> void {
  if(ppu.hd()) return plotHD(above, x, source, priority, color, false, false);
  if(priority > above[x].priority) above[x] = {source, priority, color};
}

auto PPU::Line::plotBelow(uint x, uint source, uint priority, uint color) -> void {
  if(ppu.hd()) return plotHD(below, x, source, priority, color, false, false);
  if(priority > below[x].priority) below[x] = {source, priority, color};
}

//todo: name these variables more clearly ...
auto PPU::Line::plotHD(Pixel* pixel, uint x, uint source, uint priority, uint color, bool hires, bool subpixel) -> void {
  auto scale = ppu.hdScale();
  int xss = hires && subpixel ? scale / 2 : 0;
  int ys = ppu.interlace() && ppu.field() ? scale / 2 : 0;
  if(priority > pixel[x * scale + xss + ys * 256 * scale].priority) {
    Pixel p = {source, priority, color};
    int xsm = hires && !subpixel ? scale / 2 : scale;
    int ysm = ppu.interlace() && !ppu.field() ? scale / 2 : scale;
    for(int xs = xss; xs < xsm; xs++) {
      pixel[x * scale + xs + ys * 256 * scale] = p;
    }
    int size = sizeof(Pixel) * (xsm - xss);
    Pixel* source = &pixel[x * scale + xss + ys * 256 * scale];
    for(int yst = ys + 1; yst < ysm; yst++) {
      memcpy(&pixel[x * scale + xss + yst * 256 * scale], source, size);
    }
  }
}
