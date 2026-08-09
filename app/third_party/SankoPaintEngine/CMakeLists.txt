qt_add_library(SankoPaintEngine STATIC
    src/Brush.cpp
    src/Brush.h
    src/GrainField.h
    src/WetEdges.h
    src/BrushEngineIntegration.cpp
    src/BrushEngineIntegration.h
    src/ColorStrokeBuffer.cpp
    src/ColorStrokeBuffer.h
    src/DualBrushCompositor.cpp
    src/DualBrushCompositor.h
    src/GpuStampRenderer.cpp
    src/GpuStampRenderer.h
    src/PixelCompositor.cpp
    src/PixelCompositor.h
    src/PressureCurve.cpp
    src/PressureCurve.h
    src/StrokeBuilder.cpp
    src/StrokeBuilder.h
    src/TiledImage.cpp
    src/TiledImage.h
    src/ViewportCamera.cpp
    src/ViewportCamera.h
)

target_include_directories(SankoPaintEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(SankoPaintEngine
    PUBLIC Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::GuiPrivate)

qt_add_shaders(SankoPaintEngine sankotv_brush_shaders
    PREFIX "/shaders"
    FILES
        shaders/stamp.vert
        shaders/stamp.frag
        shaders/color_stamp.vert
        shaders/color_stamp.frag)

qt_add_shaders(SankoPaintEngine sankotv_publication_shaders
    PREFIX "/shaders"
    GLSL "150"
    FILES
        shaders/publish.vert
        shaders/publish.frag
        shaders/dual_publish.vert
        shaders/dual_publish.frag)

add_library(SankoPaint::Engine ALIAS SankoPaintEngine)
