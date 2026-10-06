#pragma once
// A movie in the viewer: core's MoviePlayer decodes it, this shows its frames (a D3D11 texture) with play / pause and
// a time bar.
#include "movie.hpp"
#include "zoom_view.hpp"

#include <d3d11.h>

#include <memory>
#include <string>

class MovieView {
public:
    explicit MovieView(ID3D11Device* device) : device_(device) {}
    ~MovieView();
    MovieView(const MovieView&) = delete;
    MovieView& operator=(const MovieView&) = delete;

    // Fills the rest of the window with `file` (opened when it changes, paused on its first frame): controls, then the
    // picture (zoomable like the texture viewer).
    void draw(const std::string& file);
    void close();  // stops decoding

private:
    void upload(const remod::MoviePlayer::Frame& frame);

    ID3D11Device* device_;
    ID3D11Texture2D* texture_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    unsigned width_ = 0, height_ = 0;
    std::string file_;
    std::unique_ptr<remod::MoviePlayer> player_;
    remod::MoviePlayer::Frame frame_;
    ZoomPan view_;
    bool scrubbing_ = false;  // the time bar is being dragged
    float scrub_ = 0;
};
