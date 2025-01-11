/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <CommCtrl.h>
#include <ctime>

#ifdef LAUNCHER_PERSONALITY_MAIN
#include <shobjidl.h>

#include "launcher.rc.h"

#include <ShellScalingApi.h>

#include <winrt/Windows.Storage.Streams.h>

#include "CitiLaunch/BackdropBrush.g.h"
#include "winrt/Microsoft.Graphics.Canvas.Effects.h"

#include <DirectXMath.h>
#include <roapi.h>

#include <CfxState.h>
#include <HostSharedData.h>

#include <boost/algorithm/string.hpp>

#include <d2d1effects.h>
#include <d2d1_1.h>
#pragma comment(lib, "dxguid.lib")

#include <windows.graphics.effects.h>
#include <windows.graphics.effects.interop.h>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "delayimp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")

using namespace ABI::Windows::Graphics::Effects;

struct CompositionEffect : winrt::implements
	<
		CompositionEffect,
		winrt::Windows::Graphics::Effects::IGraphicsEffectSource, 
		winrt::Windows::Graphics::Effects::IGraphicsEffect,
		ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop
	>
{
	CompositionEffect(const GUID& effectId)
	{
		m_effectId = effectId;
	}

	winrt::hstring Name()
	{
		return m_name;
	}

	void Name(winrt::hstring const& name)
	{
		m_name = name;
	}

	template<typename T>
	void SetProperty(const std::string& name, const T& value, GRAPHICS_EFFECT_PROPERTY_MAPPING mapping = GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT)
	{
		m_properties.emplace_back(name, winrt::box_value(value), mapping);
	}

	template<int N>
	void SetProperty(const std::string& name, const float (&value)[N], GRAPHICS_EFFECT_PROPERTY_MAPPING mapping = GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT)
	{
		m_properties.emplace_back(name, winrt::Windows::Foundation::PropertyValue::CreateSingleArray(winrt::array_view<const float>{ (float*)&value, (float*)&value + N }), mapping);
	}

	template<>
	void SetProperty(const std::string& name, const winrt::Windows::UI::Color& color, GRAPHICS_EFFECT_PROPERTY_MAPPING mapping)
	{
		float values[] = { color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, color.A / 255.0f };
		SetProperty(name, values, mapping);
	}

	template<>
	void SetProperty(const std::string& name, const winrt::Microsoft::Graphics::Canvas::Effects::Matrix5x4& value, GRAPHICS_EFFECT_PROPERTY_MAPPING mapping)
	{
		float mat[5 * 4];
		memcpy(mat, &value, sizeof(mat));
		SetProperty(name, mat, mapping);
	}

	template<>
	void SetProperty(const std::string& name, const winrt::Windows::Foundation::Numerics::float3x2& value, GRAPHICS_EFFECT_PROPERTY_MAPPING mapping)
	{
		float mat[3 * 2];
		memcpy(mat, &value, sizeof(mat));
		SetProperty(name, mat, mapping);
	}

	void AddSource(const winrt::Windows::Graphics::Effects::IGraphicsEffectSource& source)
	{
		m_sources.push_back(source);
	}

	virtual HRESULT __stdcall GetEffectId(GUID* id) override
	{
		*id = m_effectId;
		return S_OK;
	}

	virtual HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR name, UINT* index, GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) override
	{
		auto nname = ToNarrow(name);

		auto entry = std::find_if(m_properties.begin(), m_properties.end(), [&nname](const auto& property)
		{
			return nname == std::get<0>(property);
		});

		if (entry != m_properties.end())
		{
			*index = entry - m_properties.begin();
			*mapping = std::get<2>(*entry);
			return S_OK;
		}

		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}

	virtual HRESULT __stdcall GetPropertyCount(UINT* count) override
	{
		*count = m_properties.size();
		return S_OK;
	}

	virtual HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) override
	{
		std::get<1>(m_properties[index]).as<ABI::Windows::Foundation::IPropertyValue>().copy_to(value);
		return S_OK;
	}

	virtual HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) override
	{
		m_sources[index].as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().copy_to(source);
		return S_OK;
	}

	virtual HRESULT __stdcall GetSourceCount(UINT* count) override
	{
		*count = UINT(m_sources.size());
		return S_OK;
	}

private:
	GUID m_effectId;

	winrt::hstring m_name = L"";

	std::vector<std::tuple<std::string, winrt::Windows::Foundation::IInspectable, GRAPHICS_EFFECT_PROPERTY_MAPPING>> m_properties;
	std::vector<winrt::Windows::Graphics::Effects::IGraphicsEffectSource> m_sources;
};

static class DPIScaler
{
public:
	DPIScaler()
	{
		// Default DPI is 96 (100%)
		dpiX = 96;
		dpiY = 96;
	}

	void SetScale(UINT dpiX, UINT dpiY)
	{
		this->dpiX = dpiX;
		this->dpiY = dpiY;
	}

	int ScaleX(int x)
	{
		return MulDiv(x, dpiX, 96);
	}

	int ScaleY(int y)
	{
		return MulDiv(y, dpiY, 96);
	}

private:
	UINT dpiX, dpiY;
} g_dpi;

using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;

struct TenUI
{
	DesktopWindowXamlSource uiSource{ nullptr };

	winrt::Windows::UI::Xaml::UIElement snailContainer{ nullptr };
	winrt::Windows::UI::Xaml::Controls::TextBlock topStatic{ nullptr };
	winrt::Windows::UI::Xaml::Controls::TextBlock bottomStatic{ nullptr };
	winrt::Windows::UI::Xaml::Controls::ProgressBar progressBar{ nullptr };
};

//static thread_local struct  
static struct
{
	HWND rootWindow;
	HWND topStatic;
	HWND bottomStatic;
	HWND progressBar;
	HWND cancelButton;

	HWND tenWindow;

	UINT taskbarMsg;

	bool tenMode;
	bool canceled;

	std::unique_ptr<TenUI> ten;

	ITaskbarList3* tbList;

	wchar_t topText[512];
	wchar_t bottomText[512];
} g_uui;

HWND UI_GetWindowHandle()
{
	return g_uui.rootWindow;
}

HFONT UI_CreateScaledFont(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic,
	DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
	DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
	LOGFONT logFont;
	
	memset(&logFont, 0, sizeof(LOGFONT));
	logFont.lfHeight = g_dpi.ScaleY(cHeight);
	logFont.lfWidth = cWidth;
	logFont.lfEscapement = cEscapement;
	logFont.lfOrientation = cOrientation;
	logFont.lfWeight = cWeight;
	logFont.lfItalic = bItalic;
	logFont.lfUnderline = bUnderline;
	logFont.lfStrikeOut = bStrikeOut;
	logFont.lfCharSet = iCharSet;
	logFont.lfOutPrecision = 8;
	logFont.lfClipPrecision = iClipPrecision;
	logFont.lfQuality = iQuality;
	logFont.lfPitchAndFamily = iPitchAndFamily;
	wcscpy_s(logFont.lfFaceName, pszFaceName);
	return CreateFontIndirect(&logFont);
}

static std::wstring g_mainXaml = LR"(
<Grid
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    xmlns:d="http://schemas.microsoft.com/expression/blend/2008"
    xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"
	xmlns:local="using:CitiLaunch"
    mc:Ignorable="d">

    <Grid Width="525" Height="525">
        <Grid.Resources>
            <!--<ThemeShadow x:Name="SharedShadow">
            </ThemeShadow>-->
        </Grid.Resources>
        <Grid x:Name="BackdropGrid" />
		<SwapChainPanel x:Name="Overlay" />
        <StackPanel Orientation="Vertical" VerticalAlignment="Center">)"
#if defined(GTA_FIVE)
	R"(
            <Viewbox Height="150" Margin="0,0,0,15" RenderTransformOrigin="0.5,0.5">
<Path Data="M 14.253 6.067 L 10.005 6.634 6.253 10.990 L 2.500 15.346 1.297 21.423 L 0.093 27.500 0.082 94 L 0.071 160.500 1.494 166.500 L 2.917 172.500 6.444 175.750 L 9.971 179 11.909 179 L 13.847 179 16.992 181.647 L 20.138 184.294 24.319 185.103 C 26.619 185.549, 32.550 185.930, 37.500 185.951 L 46.500 185.989 50.149 184.975 L 53.798 183.962 55.327 180.231 L 56.856 176.500 61.678 171.367 L 66.500 166.234 74.704 163.617 L 82.909 161 85.704 161.033 C 87.242 161.051, 90.975 161.910, 94 162.942 L 99.500 164.818 100.624 166.973 C 101.242 168.158, 103.010 170.445, 104.553 172.055 L 107.358 174.983 108.077 185.742 L 108.795 196.500 110 199.124 L 111.206 201.747 109.997 204.005 L 108.789 206.264 108.027 218.188 L 107.266 230.113 104.658 232.835 C 103.224 234.332, 101.569 236.938, 100.980 238.626 L 99.910 241.696 94.605 246.277 L 89.299 250.857 81.742 252.418 C 77.585 253.277, 72.929 254.457, 71.395 255.040 L 68.604 256.101 61.052 254.583 C 56.898 253.748, 48.577 252.603, 42.560 252.040 L 31.621 251.015 27.702 248.421 C 25.546 246.995, 21.670 245.200, 19.089 244.434 L 14.395 243.040 12.204 240.520 L 10.013 238 6.941 238 C 5.251 238, 2.998 238.466, 1.934 239.035 L 0 240.070 -0 264.900 L -0 289.730 2.227 292.615 C 5.403 296.731, 6.978 297.825, 11.500 299.058 C 13.700 299.658, 17.075 301.160, 19 302.395 C 23.690 305.403, 25.162 305.888, 32.958 306.994 L 39.627 307.940 45.563 310.707 L 51.500 313.474 75 313.472 L 98.500 313.471 105.500 310.341 C 109.350 308.620, 114.075 306.890, 116 306.496 L 119.500 305.781 122.500 303.535 C 124.150 302.299, 127.323 300.360, 129.551 299.226 L 133.602 297.163 140.801 289.992 L 148 282.821 148 281.500 L 148 280.178 151.082 277.585 L 154.164 274.991 155.136 271.468 L 156.109 267.945 158.931 264.723 L 161.753 261.500 162.476 256.208 L 163.199 250.917 165.811 247.813 L 168.422 244.710 169.290 228.653 L 170.157 212.596 171.579 209.848 L 173 207.099 173 204.364 C 173 202.859, 172.335 198.899, 171.522 195.564 L 170.044 189.500 169.229 173.890 L 168.414 158.281 165.754 155.120 L 163.095 151.959 162.404 145.851 L 161.712 139.742 158.991 136.621 C 157.494 134.904, 155.934 132.375, 155.525 131 C 155.115 129.625, 153.547 127.150, 152.041 125.500 C 150.535 123.850, 148.672 121.556, 147.901 120.403 C 146.409 118.170, 139.928 112.551, 132.793 107.305 L 128.447 104.110 121.749 103.040 C 118.065 102.451, 113.802 101.489, 112.275 100.902 L 109.500 99.834 97.587 99.646 L 85.673 99.457 83.087 100.837 L 80.500 102.216 73.078 103.079 L 65.656 103.942 62.530 106.971 L 59.405 110 57.702 110 L 56 110 56.015 87.250 L 56.031 64.500 56.765 63.500 L 57.500 62.500 97.500 62.500 L 137.500 62.500 143.204 60.196 L 148.907 57.891 151.973 53.722 L 155.038 49.554 154.769 28.527 L 154.500 7.500 144 6.820 C 130.634 5.954, 19.912 5.312, 14.253 6.067 M 237.500 7.935 C 236.400 8.380, 233.475 9.059, 231 9.444 C 228.525 9.828, 223.350 11.088, 219.500 12.244 L 212.500 14.344 204.939 22.025 L 197.379 29.706 194.296 38.603 L 191.212 47.500 189.952 62 L 188.692 76.500 188.767 92 L 188.843 107.500 190.882 115.257 L 192.922 123.014 197.414 132.257 L 201.907 141.500 207.119 148.364 L 212.330 155.229 218.602 160.401 L 224.873 165.574 233.187 168.470 L 241.500 171.365 405.500 172.222 L 569.500 173.080 604.500 172.476 C 623.750 172.144, 642.351 171.634, 645.836 171.343 L 652.173 170.814 654.336 172.846 L 656.500 174.878 660.500 175.829 C 665.310 176.972, 676.527 182.044, 683.750 186.343 L 689 189.467 689 190.661 L 689 191.854 694.180 195.677 C 697.029 197.780, 702.994 203.100, 707.435 207.500 L 715.510 215.500 720.774 223 C 723.668 227.125, 727.407 232.331, 729.081 234.570 L 732.126 238.640 735.034 250.070 C 736.634 256.356, 738.939 266.225, 740.157 272 L 742.370 282.500 742.293 292.500 L 742.216 302.500 740.519 313.947 L 738.823 325.393 735.830 332.947 L 732.836 340.500 730.234 344 C 728.802 345.925, 721.370 353.575, 713.717 361 L 699.803 374.500 691.652 380.393 L 683.500 386.286 675.277 390.122 L 667.054 393.958 659.514 395.933 C 655.367 397.020, 648.267 398.405, 643.737 399.011 C 639.207 399.618, 631 401.088, 625.500 402.277 L 615.500 404.440 412.636 404.470 L 209.771 404.500 208.136 406.860 C 206.526 409.182, 203.548 412.261, 199.939 415.334 L 198.105 416.895 197.030 420.887 L 195.955 424.879 193.905 425.530 L 191.855 426.181 192.632 427.840 C 193.812 430.364, 193.858 433.476, 194.962 584.500 C 195.529 662.050, 196.248 748.225, 196.559 776 L 197.125 826.500 201.079 834.687 L 205.034 842.873 209.122 846.373 L 213.210 849.872 225.355 855.038 L 237.500 860.204 252 861.042 C 259.975 861.503, 274.056 861.907, 283.292 861.940 L 300.084 862 307.532 860.462 C 311.628 859.616, 318.419 857.801, 322.622 856.429 L 330.264 853.934 337.157 849.909 L 344.049 845.883 346.470 842.192 C 347.802 840.161, 351.227 832.650, 354.083 825.500 L 359.275 812.500 361.638 809.945 L 364 807.391 363.994 797.445 L 363.987 787.500 360.994 785.797 L 358 784.093 358 780.747 L 358 777.400 359.200 776.200 C 359.860 775.540, 361.210 775, 362.200 775 L 364 775 364 673.598 L 364 572.196 367.149 571.098 L 370.298 570 468.494 570 L 566.690 570 568.500 568 L 570.310 566 572.623 566 L 574.935 566 578.881 571.250 C 581.052 574.137, 586.618 582.575, 591.251 590 C 595.884 597.425, 603.637 609.800, 608.481 617.500 C 613.324 625.200, 622.915 640.500, 629.795 651.500 C 636.675 662.500, 645.310 676.225, 648.985 682 C 652.659 687.775, 657.626 695.650, 660.022 699.500 C 662.419 703.350, 667.374 711.225, 671.035 717 C 674.696 722.775, 682.768 735.600, 688.973 745.500 C 695.179 755.400, 703.275 768.225, 706.965 774 C 718.882 792.650, 741.328 827.991, 743.803 832 C 744.652 833.375, 749.431 838.931, 754.423 844.346 L 763.500 854.191 767.500 856.544 C 769.700 857.837, 774.200 859.647, 777.500 860.564 L 783.500 862.232 833.500 862.086 L 883.500 861.941 889 860.931 C 892.025 860.376, 895.468 859.717, 896.651 859.466 L 898.803 859.011 904.424 854.432 L 910.045 849.853 912.885 844.177 C 914.447 841.054, 916.249 836.250, 916.891 833.500 L 918.058 828.500 917.017 818.562 C 916.444 813.096, 915.536 807.021, 914.999 805.062 L 914.023 801.500 893.761 770.760 C 882.618 753.852, 865.400 727.651, 855.500 712.534 C 832.907 678.035, 801.506 630.163, 782.669 601.500 C 774.717 589.400, 763.025 571.603, 756.686 561.951 L 745.161 544.403 746.127 535.951 C 746.658 531.303, 747.576 526.265, 748.165 524.757 L 749.237 522.013 752.869 521.022 L 756.500 520.031 772.273 510.575 C 809.557 488.222, 833.667 468.643, 848.216 448.902 C 855.841 438.557, 877.912 405.446, 878.623 403.286 C 878.947 402.304, 881.177 397.450, 883.579 392.500 L 887.947 383.500 889.963 375.500 C 891.072 371.100, 893.571 359.850, 895.516 350.500 L 899.052 333.500 900.003 321.500 L 900.955 309.500 900.977 272.889 L 901 236.278 898.890 225.931 L 896.779 215.584 890.805 199.542 C 872.673 150.852, 870.497 146.984, 841.589 112.066 L 825.500 92.632 813 81.689 C 783.798 56.123, 777.884 52.086, 754.500 41.758 L 739.500 35.133 727.500 31.600 C 720.900 29.657, 711.675 27.169, 707 26.072 L 698.500 24.076 653.500 17.938 L 608.500 11.800 563 10.997 C 513.514 10.122, 436.874 9.306, 327.500 8.489 C 289 8.201, 253.450 7.777, 248.500 7.545 L 239.500 7.124 237.500 7.935 M 27.886 358.177 L 19.942 362.153 15.057 367.038 L 10.173 371.923 8.010 377.711 C 6.821 380.895, 5.094 384.336, 4.174 385.357 L 2.500 387.213 2.893 515.357 C 3.109 585.836, 3.589 688.050, 3.959 742.500 L 4.632 841.500 6.320 846 L 8.008 850.500 10.863 853 C 12.434 854.375, 14.228 856.476, 14.852 857.670 L 15.985 859.840 19.534 861.011 L 23.082 862.182 24.540 861.623 L 25.997 861.064 28.998 863.998 L 32 866.933 32 868.466 L 32 870 86.500 870 L 141 870 141 868.223 C 141 867.245, 141.658 865.505, 142.463 864.357 L 143.925 862.269 148.701 861.730 L 153.476 861.192 154.488 859.346 C 155.045 858.331, 157.357 855.700, 159.627 853.500 C 161.897 851.300, 164.301 848.439, 164.970 847.143 L 166.185 844.786 166.748 618.617 L 167.311 392.449 165.222 386.413 L 163.133 380.377 159.567 374.961 C 157.605 371.983, 156 369.246, 156 368.879 C 156 367.922, 142.117 357.530, 138.215 355.566 L 134.930 353.913 85.380 354.057 L 35.830 354.201 27.886 358.177" Fill="#3c67c4" />
                <Viewbox.RenderTransform>
                    <ScaleTransform ScaleX="1" />
                </Viewbox.RenderTransform>)"
#elif defined(IS_RDR3)
	R"(
			<Viewbox Height="150" Margin="0,0,0,15">
				<Grid>
				<Path Data="F1 M 38.56,38.56 L 779.52,38.56 779.52,1019.52 38.56,1019.52 z"  Fill="#00000000" />
				<Path Data="F1 M 153.23,78.44 L 154.67,77.16 153.23,75.72 153.23,78.44 153.23,78.44 z"  Fill="#ffffffff" />
				<Path Data="F1 M 677.12,48.82 L 523.2,98.61 516.32,118.63 673.43,67.71 677.12,48.82 677.12,48.82 z"  Fill="#ffffffff" />
				<Path Data="F1 M 666.07,105.5 L 668.63,92.37 507.35,144.73 502.7,158.34 666.07,105.5 666.07,105.5 z"  Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L -13.94,40.99 -32.52,105.83 -42.61,153.07 116.91,176.77 134.69,107.28 166.24,-53.8
					 0,0 0.16,0 z" RenderTransform="1,0,0,1,496.62,175.63" Fill="#ffffffff" />
				<Path Data="F1 M 670.55,38.73 L 543.7,38.73 527.85,84.84 670.55,38.73 z"  Fill="#ffffffff" />
				<Path Data="F1 M 311.47,167.46 L 152.43,218.86 151.95,224.46 151.95,236.79 310.51,185.55 311.47,167.46 z"  Fill="#ffffffff" />
				<Path Data="F1 M 308.91,221.26 L 309.55,209.09 151.95,260.01 151.95,272.18 308.91,221.26 308.91,221.26 z"  Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L -9.45,-0.96 19.22,-146.18 -133.89,-168.92 -144.78,-118.16 -164.64,-6.72 -266.82,-6.72
					 -245.52,-296.05 -245.52,-404.93 -244.72,-425.59 -401.04,-375.15 -401.04,-265.63 -405.2,-256.34 -404.88,-247.7
					 -409.05,-182.05 -412.09,-168.76 -411.77,-133.06 -411.77,358.34 94.18,358.34 94.18,326.48 116.76,-5.28
					 34.44,0 0,0 0,0 z
					M -25.14,211.04 L -272.27,211.04 -264.26,171.33 -264.26,143.31 -272.27,124.73 -25.14,124.73 -27.87,163.16
					 -25.14,211.04 z" RenderTransform="1,0,0,1,552.99,662.7" Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L 2.88,-108.56 -156.31,-113.84 -155.83,-101.99 -157.12,-79.41 -157.12,37.47 -158.24,51.08
					 0,0 0,0 z" RenderTransform="1,0,0,1,311.79,155.13" Fill="#ffffffff" />
				</Grid>
)"
#elif defined(GTA_NY)
								 R"(
			<Viewbox Height="150" Margin="0,0,0,15">
				<Grid>
				<Path Data="M26,145L54.571,145C54.952,144.905 55.143,144.714 55.143,144.429L55.143,69C43.714,57.286 33.905,47.476 25.714,39.571L25.429,39.571L25.429,144.429C25.524,144.81 25.714,145 26,145ZM54.857,57.857L55.143,57.857L55.143,54.143C43.714,42.429 33.905,32.619 25.714,24.714L25.429,24.714L25.429,28.429C36.857,40.048 46.667,49.857 54.857,57.857ZM54.857,43L55.143,43L55.143,31.571C46.857,23 38,14.143 28.571,5L26,5C25.619,5 25.429,5.19 25.429,5.571L25.429,13.571C36.857,25.19 46.667,35 54.857,43ZM57.714,30.429L124,30.429C124.381,30.333 124.571,30.143 124.571,29.857L124.571,5.571C124.571,5.19 124.381,5 124,5L32.571,5L32.571,5.286C41.714,14.619 50.095,23 57.714,30.429Z"  Fill="#ffffffff" />
				</Grid>
)"
#endif
R"(         </Viewbox>
            <TextBlock x:Name="static1" Text=" " TextAlignment="Center" Foreground="#ffffffff" FontSize="24" />
			<Grid Margin="0,15,0,15">
				<ProgressBar x:Name="progressBar" Foreground="White" Width="250" />
			</Grid>
            <TextBlock x:Name="static2" Text=" " TextAlignment="Center" Foreground="#ffeeeeee" FontSize="18" />
			<StackPanel Orientation="Horizontal" HorizontalAlignment="Center" x:Name="snailContainer" Visibility="Collapsed">
				<TextBlock TextAlignment="Center" Foreground="#ddeeeeee" FontSize="14" Width="430" TextWrapping="Wrap">
					🐌 RedM game storage downloads are peer-to-peer and may be slower than usual downloads. Please be patient.
				</TextBlock>
			</StackPanel>
        </StackPanel>
    </Grid>
</Grid>
)";

struct BackdropBrush : winrt::CitiLaunch::implementation::BackdropBrushT<BackdropBrush>
{
	BackdropBrush() = default;

	void OnConnected();
	void OnDisconnected();

	winrt::Windows::UI::Composition::CompositionPropertySet ps{ nullptr };
};

void BackdropBrush::OnConnected()
{
	if (!CompositionBrush())
	{
		//
		// !NOTE! if trying to change the following code (add extra effects, change effects, etc.)
		// 
		// CLSIDs and properties are from Win2D:
		//   https://github.com/microsoft/Win2D/tree/99ce19f243c6a6332f0ea312cd29fc3c785a540b/winrt/lib/effects/generated
		//
		// The .h files show the CLSID used, the .cpp files the properties with names, type, mapping and default values.
		// 
		// Properties *have* to be set in the original order - initial deserialization (at least in wuceffects.dll 10.0.22000)
		// will check all properties before checking the name mapping. Also, `Source` properties are mapped to the AddSource
		// list, instead of being a real property.
		//
		auto effect = CompositionEffect(CLSID_D2D1Flood);

#ifdef GTA_FIVE
		effect.SetProperty("Color", winrt::Windows::UI::ColorHelper::FromArgb(255, 0x16, 0x19, 0x23));
#elif defined(IS_RDR3)
		effect.SetProperty("Color", winrt::Windows::UI::ColorHelper::FromArgb(255, 186, 2, 2));
#elif defined(GTA_NY)
		effect.SetProperty("Color", winrt::Windows::UI::ColorHelper::FromArgb(255, 0x4D, 0xA6, 0xD3));
#endif

		winrt::Windows::UI::Composition::CompositionEffectSourceParameter sp{ L"layer" };
		winrt::Windows::UI::Composition::CompositionEffectSourceParameter sp2{ L"rawImage" };

		auto mat2d = winrt::Windows::Foundation::Numerics::float3x2{};

		using namespace DirectX;
		auto matrix = XMMatrixTransformation2D(XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f), XMVectorSet(0.5f, 0.5f, 0.0f, 0.0f), 0.2, XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f));
		XMStoreFloat3x2(&mat2d, matrix);

		auto layer = CompositionEffect(CLSID_D2D12DAffineTransform);
		layer.AddSource(sp2);
		layer.Name(L"xform");

		layer.SetProperty("InterpolationMode", uint32_t(D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR));
		layer.SetProperty("BorderMode", uint32_t(D2D1_BORDER_MODE_SOFT));
		layer.SetProperty("TransformMatrix", mat2d);
		layer.SetProperty("Sharpness", 0.0f);

		auto mat = winrt::Microsoft::Graphics::Canvas::Effects::Matrix5x4();
		memset(&mat, 0, sizeof(mat));
		mat.M44 = 1.0f;

#ifdef GTA_FIVE
		mat.M11 = 1.0f;
		mat.M22 = 1.0f;
		mat.M33 = 1.0f;
		mat.M44 = 0.03f;
#elif defined(IS_RDR3) || defined(GTA_NY)
		mat.M11 = 1.0f;
		mat.M22 = 1.0f;
		mat.M33 = 1.0f;
		mat.M44 = 0.15f;
#endif

		auto layerColor = CompositionEffect(CLSID_D2D1ColorMatrix);
		layerColor.AddSource(layer);
		layerColor.SetProperty("ColorMatrix", mat);
		layerColor.SetProperty("AlphaMode", uint32_t(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED), GRAPHICS_EFFECT_PROPERTY_MAPPING_COLORMATRIX_ALPHA_MODE);
		layerColor.SetProperty("ClampOutput", false);

		auto compEffect = CompositionEffect(CLSID_D2D1Composite);
		compEffect.SetProperty("Mode", uint32_t(D2D1_COMPOSITE_MODE_SOURCE_OVER));
		compEffect.AddSource(effect);
		compEffect.AddSource(layerColor);

		auto hRsc = FindResource(GetModuleHandle(NULL), MAKEINTRESOURCE(IDM_BACKDROP), L"MEOW");
		auto resSize = SizeofResource(GetModuleHandle(NULL), hRsc);
		auto resData = LoadResource(GetModuleHandle(NULL), hRsc);

		auto resPtr = static_cast<const uint8_t*>(LockResource(resData));

		auto iras = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
		auto dw = winrt::Windows::Storage::Streams::DataWriter{ iras };
		dw.WriteBytes(winrt::array_view<const uint8_t>{resPtr, resPtr + resSize});

		auto iao = dw.StoreAsync();
		while (iao.Status() != winrt::Windows::Foundation::AsyncStatus::Completed)
		{
			Sleep(0);
		}

		iras.Seek(0);

		auto surf = winrt::Windows::UI::Xaml::Media::LoadedImageSurface::StartLoadFromStream(iras);

		auto cb = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateSurfaceBrush();
		cb.Surface(surf);
		//cb.Stretch(winrt::Windows::UI::Composition::CompositionStretch::UniformToFill);
		cb.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);

		auto ef = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateEffectFactory(compEffect, { L"xform.TransformMatrix" });
		auto eb = ef.CreateBrush();
		eb.SetSourceParameter(L"rawImage", cb);

		using namespace std::chrono_literals;

		auto kfa = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateVector2KeyFrameAnimation();
		kfa.InsertKeyFrame(0.0f, { 0.0f, 0.0f });
		kfa.InsertKeyFrame(0.25f, { 0.0f, -300.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(0.5f, { -300.0f, -300.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(0.75f, { -300.0f, 0.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(1.0f, { 0.0f, 0.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.Duration(60s);
		kfa.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);
		kfa.Target(L"xlate");

		auto ag = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateAnimationGroup();
		ag.Add(kfa);

		ps = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreatePropertySet();
		ps.InsertVector2(L"xlate", { 0.0f, 0.0f });
		ps.StartAnimationGroup(ag);

		auto ca = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateExpressionAnimation();
		ca.SetReferenceParameter(L"ps", ps);
		ca.SetMatrix3x2Parameter(L"rot", mat2d);
		ca.Expression(L"Matrix3x2.CreateFromTranslation(ps.xlate) * rot");

		eb.StartAnimation(L"xform.TransformMatrix", ca);

		CompositionBrush(eb);
	}
}

void BackdropBrush::OnDisconnected()
{
	if (CompositionBrush())
	{
		CompositionBrush(nullptr);
	}
}

#include <wrl.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <windows.ui.xaml.media.dxinterop.h>

using Microsoft::WRL::ComPtr;

const BYTE g_PixyShader[] =
{
     68,  88,  66,  67, 115,  61, 
    165, 134, 202, 176,  67, 148, 
    204, 160, 214, 207, 231, 188, 
    224, 101,   1,   0,   0,   0, 
     48,  10,   0,   0,   5,   0, 
      0,   0,  52,   0,   0,   0, 
     36,   1,   0,   0, 124,   1, 
      0,   0, 176,   1,   0,   0, 
    180,   9,   0,   0,  82,  68, 
     69,  70, 232,   0,   0,   0, 
      1,   0,   0,   0,  68,   0, 
      0,   0,   1,   0,   0,   0, 
     28,   0,   0,   0,   0,   4, 
    255, 255,   0,   1,   0,   0, 
    192,   0,   0,   0,  60,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,   1,   0,   0,   0, 
     80, 115,  67,  98, 117, 102, 
      0, 171,  60,   0,   0,   0, 
      2,   0,   0,   0,  92,   0, 
      0,   0,  16,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0, 140,   0,   0,   0, 
      0,   0,   0,   0,   8,   0, 
      0,   0,   0,   0,   0,   0, 
    152,   0,   0,   0,   0,   0, 
      0,   0, 168,   0,   0,   0, 
      8,   0,   0,   0,   4,   0, 
      0,   0,   2,   0,   0,   0, 
    176,   0,   0,   0,   0,   0, 
      0,   0, 105,  82, 101, 115, 
    111, 108, 117, 116, 105, 111, 
    110,   0,   1,   0,   3,   0, 
      1,   0,   2,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    105,  84, 105, 109, 101,   0, 
    171, 171,   0,   0,   3,   0, 
      1,   0,   1,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     77, 105,  99, 114, 111, 115, 
    111, 102, 116,  32,  40,  82, 
     41,  32,  72,  76,  83,  76, 
     32,  83, 104,  97, 100, 101, 
    114,  32,  67, 111, 109, 112, 
    105, 108, 101, 114,  32,  49, 
     48,  46,  49,   0,  73,  83, 
     71,  78,  80,   0,   0,   0, 
      2,   0,   0,   0,   8,   0, 
      0,   0,  56,   0,   0,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,   3,   0,   0,   0, 
      0,   0,   0,   0,  15,   0, 
      0,   0,  68,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   3,   0,   0,   0, 
      1,   0,   0,   0,   3,   3, 
      0,   0,  83,  86,  95,  80, 
     79,  83,  73,  84,  73,  79, 
     78,   0,  84,  69,  88,  67, 
     79,  79,  82,  68,   0, 171, 
    171, 171,  79,  83,  71,  78, 
     44,   0,   0,   0,   1,   0, 
      0,   0,   8,   0,   0,   0, 
     32,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,   0,  15,   0,   0,   0, 
     83,  86,  95,  84,  65,  82, 
     71,  69,  84,   0, 171, 171, 
     83,  72,  68,  82, 252,   7, 
      0,   0,  64,   0,   0,   0, 
    255,   1,   0,   0,  89,   0, 
      0,   4,  70, 142,  32,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,  98,  16,   0,   3, 
     50,  16,  16,   0,   1,   0, 
      0,   0, 101,   0,   0,   3, 
    242,  32,  16,   0,   0,   0, 
      0,   0, 104,   0,   0,   2, 
      5,   0,   0,   0,  56,   0, 
      0,  11,  50,   0,  16,   0, 
      0,   0,   0,   0, 166, 138, 
     32,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   2,  64, 
      0,   0, 205, 204, 204,  61, 
     66,  96, 101,  63,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     77,   0,   0,   6,  18,   0, 
     16,   0,   0,   0,   0,   0, 
      0, 208,   0,   0,  10,   0, 
     16,   0,   0,   0,   0,   0, 
     50,   0,   0,  15, 194,   0, 
     16,   0,   0,   0,   0,   0, 
     86,  17,  16,   0,   1,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0, 128, 191, 
      0,   0, 128,  63,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    128,  63,   0,   0,   0,   0, 
     54,   0,   0,   8,  50,   0, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  48,   0,   0,   1, 
     33,   0,   0,   7,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     26,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
     66,   0,   0,   0,   3,   0, 
      4,   3,  42,   0,  16,   0, 
      1,   0,   0,   0,  43,   0, 
      0,   5,  66,   0,  16,   0, 
      1,   0,   0,   0,  26,   0, 
     16,   0,   1,   0,   0,   0, 
     56,   0,   0,   7, 130,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
     53, 165, 231,  64,  50,   0, 
      0,  15, 114,   0,  16,   0, 
      2,   0,   0,   0, 166,  10, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,  63, 143, 194, 117,  60, 
     10, 215,  35,  60,   0,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0, 128,  63,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0,   0,   0,  56,   0, 
      0,   7, 130,   0,  16,   0, 
      2,   0,   0,   0,  42,   0, 
     16,   0,   0,   0,   0,   0, 
     10,   0,  16,   0,   2,   0, 
      0,   0,  65,   0,   0,   5, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  50,   0, 
      0,  10, 130,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   1,   0,   0,   0, 
      1,  64,   0,   0,  53, 165, 
    231,  64,  58,   0,  16, 128, 
     65,   0,   0,   0,   1,   0, 
      0,   0,  50,   0,   0,  10, 
     18,   0,  16,   0,   3,   0, 
      0,   0,  42, 128,  32,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   1,  64,   0,   0, 
      0,   0,   0,  64,  42,   0, 
     16,   0,   1,   0,   0,   0, 
     77,   0,   0,   6,  18,   0, 
     16,   0,   3,   0,   0,   0, 
      0, 208,   0,   0,  10,   0, 
     16,   0,   3,   0,   0,   0, 
     50,   0,   0,  10, 130,   0, 
     16,   0,   1,   0,   0,   0, 
     10,   0,  16, 128,  65,   0, 
      0,   0,   3,   0,   0,   0, 
      1,  64,   0,   0, 205, 204, 
    204,  61,  58,   0,  16,   0, 
      1,   0,   0,   0,  56,   0, 
      0,   7,  34,   0,  16,   0, 
      3,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     58,   0,  16,   0,   2,   0, 
      0,   0,  14,   0,   0,   7, 
     18,   0,  16,   0,   3,   0, 
      0,   0,  26,   0,  16,   0, 
      0,   0,   0,   0,  26,   0, 
     16,   0,   2,   0,   0,   0, 
     50,   0,   0,   9,  50,   0, 
     16,   0,   2,   0,   0,   0, 
    230,  10,  16,   0,   0,   0, 
      0,   0,   6,   0,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16,   0,   3,   0,   0,   0, 
     65,   0,   0,   5,  50,   0, 
     16,   0,   3,   0,   0,   0, 
     22,   5,  16,   0,   2,   0, 
      0,   0,   0,   0,   0,   7, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,  18, 131, 249,  65, 
     65,   0,   0,   5,  66,   0, 
     16,   0,   3,   0,   0,   0, 
     58,   0,  16,   0,   1,   0, 
      0,   0,  50,   0,   0,  15, 
    114,   0,  16,   0,   4,   0, 
      0,   0,  70,   2,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 172, 197,  39,  55, 
    172, 197,  39,  55, 172, 197, 
     39,  55,   0,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0, 137,  65,  62,   0,   0, 
      0,   0,  50,   0,   0,  15, 
    194,   0,  16,   0,   3,   0, 
      0,   0,   6,   4,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0, 172, 197, 
     39,  55, 172, 197,  39,  55, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    205, 111, 245,  70, 205, 111, 
    245,  70,  16,   0,   0,  10, 
    130,   0,  16,   0,   1,   0, 
      0,   0,   2,  64,   0,   0, 
    130,  43,  85,  65, 240,  22, 
    188,  65, 153, 176, 173,  65, 
      0,   0,   0,   0,  70,   2, 
     16,   0,   4,   0,   0,   0, 
     16,   0,   0,  10, 130,   0, 
     16,   0,   2,   0,   0,   0, 
      2,  64,   0,   0,  56, 248, 
    168,  65, 127, 217, 229,  65, 
     50, 230,  62,  65,   0,   0, 
      0,   0,  70,   2,  16,   0, 
      4,   0,   0,   0,  26,   0, 
      0,   5,  18,   0,  16,   0, 
      4,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     26,   0,   0,   5,  34,   0, 
     16,   0,   4,   0,   0,   0, 
     58,   0,  16,   0,   2,   0, 
      0,   0,  14,   0,   0,   7, 
    194,   0,  16,   0,   3,   0, 
      0,   0, 166,  14,  16,   0, 
      3,   0,   0,   0,   6,   4, 
     16,   0,   4,   0,   0,   0, 
     26,   0,   0,   5, 194,   0, 
     16,   0,   3,   0,   0,   0, 
    166,  14,  16,   0,   3,   0, 
      0,   0,   0,   0,   0,   8, 
     50,   0,  16,   0,   3,   0, 
      0,   0,  22,   5,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16, 128,  65,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,  10,  50,   0,  16,   0, 
      3,   0,   0,   0,  70,   0, 
     16,   0,   3,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0, 191,   0,   0,   0, 191, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  50,   0,   0,  12, 
     50,   0,  16,   0,   3,   0, 
      0,   0, 230,  10,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 102, 102, 102,  63, 
    102, 102, 102,  63,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     70,   0,  16,   0,   3,   0, 
      0,   0,   0,   0,   0,  10, 
     50,   0,  16,   0,   3,   0, 
      0,   0,  70,   0,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 102, 102, 230, 190, 
    102, 102, 230, 190,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     56,   0,   0,  10,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16,   0,   2,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,  32,  65,   0,   0, 
     32,  65,   0,   0,   0,   0, 
      0,   0,   0,   0,  26,   0, 
      0,   5,  50,   0,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16,   0,   2,   0,   0,   0, 
     50,   0,   0,  15,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16,   0,   2,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,   0,  64,   0,   0, 
      0,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   2,  64, 
      0,   0,   0,   0, 128, 191, 
      0,   0, 128, 191,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     50,   0,   0,  14,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16, 128, 129,   0, 
      0,   0,   2,   0,   0,   0, 
      2,  64,   0,   0,  10, 215, 
     35,  60,  10, 215,  35,  60, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  70,   0,  16, 128, 
    129,   0,   0,   0,   3,   0, 
      0,   0,   0,   0,   0,   8, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  26,   0,  16, 128, 
     65,   0,   0,   0,   2,   0, 
      0,   0,  10,   0,  16,   0, 
      2,   0,   0,   0,   0,   0, 
      0,   7, 130,   0,  16,   0, 
      2,   0,   0,   0,  26,   0, 
     16,   0,   2,   0,   0,   0, 
     10,   0,  16,   0,   2,   0, 
      0,   0,  52,   0,   0,   7, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   2,   0,   0,   0, 
     52,   0,   0,   7,  18,   0, 
     16,   0,   2,   0,   0,   0, 
     26,   0,  16,   0,   2,   0, 
      0,   0,  10,   0,  16,   0, 
      2,   0,   0,   0,  50,   0, 
      0,   9, 130,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
      1,  64,   0,   0, 154, 153, 
     25,  63,  10,   0,  16,   0, 
      2,   0,   0,   0,  50,   0, 
      0,  10,  66,   0,  16,   0, 
      1,   0,   0,   0,  10,   0, 
     16, 128,  65,   0,   0,   0, 
      0,   0,   0,   0,   1,  64, 
      0,   0,   0,   0, 160,  64, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   0,   0,   0,  10, 
    194,   0,  16,   0,   1,   0, 
      0,   0, 166,  14,  16,   0, 
      1,   0,   0,   0,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    160, 192,  10, 215,  35, 188, 
     56,   0,   0,   8,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16, 128, 129,   0, 
      0,   0,   1,   0,   0,   0, 
      1,  64,   0,   0,   0,   0, 
      0,  63,  51,   0,   0,   7, 
     66,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   0,   0, 128,  63, 
     50,   0,   0,   9,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
    205, 204,  76,  61,   1,  64, 
      0,   0, 205, 204,  76,  61, 
     56,   0,   0,   7,  18,   0, 
     16,   0,   2,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
      0,   0,   0, 192,   0,   0, 
      0,   8,  66,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16, 128,  65,   0,   0,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     14,   0,   0,  10, 130,   0, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0, 128,  63,   0,   0, 
    128,  63,  10,   0,  16,   0, 
      2,   0,   0,   0,  56,  32, 
      0,   7,  66,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,  50,   0,   0,   9, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   0,   0,   0, 192, 
      1,  64,   0,   0,   0,   0, 
     64,  64,  56,   0,   0,   7, 
     66,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   1,   0,   0,   0, 
     56,   0,   0,   7,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  14,   0, 
      0,   7, 130,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   3,   0,   0,   0, 
     42,   0,  16,   0,   2,   0, 
      0,   0,  50,   0,   0,   9, 
     18,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     10,   0,  16,   0,   1,   0, 
      0,   0,  30,   0,   0,   7, 
     34,   0,  16,   0,   1,   0, 
      0,   0,  26,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   1,   0,   0,   0, 
     22,   0,   0,   1,  54,   0, 
      0,   5, 130,  32,  16,   0, 
      0,   0,   0,   0,  10,   0, 
     16,   0,   1,   0,   0,   0, 
     54,   0,   0,   8, 114,  32, 
     16,   0,   0,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0, 128,  63,   0,   0, 
      0,   0,  62,   0,   0,   1, 
     83,  84,  65,  84, 116,   0, 
      0,   0,  62,   0,   0,   0, 
      5,   0,   0,   0,   0,   0, 
      0,   0,   2,   0,   0,   0, 
     52,   0,   0,   0,   2,   0, 
      0,   0,   0,   0,   0,   0, 
      1,   0,   0,   0,   1,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,   0,   8,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0
};

const BYTE g_VertyShader[] = {
	68, 88, 66, 67, 76, 75,
	108, 163, 29, 221, 215, 151,
	233, 28, 62, 114, 145, 31,
	52, 111, 1, 0, 0, 0,
	176, 2, 0, 0, 5, 0,
	0, 0, 52, 0, 0, 0,
	128, 0, 0, 0, 180, 0,
	0, 0, 12, 1, 0, 0,
	52, 2, 0, 0, 82, 68,
	69, 70, 68, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	28, 0, 0, 0, 0, 4,
	254, 255, 0, 1, 0, 0,
	28, 0, 0, 0, 77, 105,
	99, 114, 111, 115, 111, 102,
	116, 32, 40, 82, 41, 32,
	72, 76, 83, 76, 32, 83,
	104, 97, 100, 101, 114, 32,
	67, 111, 109, 112, 105, 108,
	101, 114, 32, 49, 48, 46,
	49, 0, 73, 83, 71, 78,
	44, 0, 0, 0, 1, 0,
	0, 0, 8, 0, 0, 0,
	32, 0, 0, 0, 0, 0,
	0, 0, 6, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 1, 1, 0, 0,
	83, 86, 95, 86, 69, 82,
	84, 69, 88, 73, 68, 0,
	79, 83, 71, 78, 80, 0,
	0, 0, 2, 0, 0, 0,
	8, 0, 0, 0, 56, 0,
	0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 3, 0,
	0, 0, 0, 0, 0, 0,
	15, 0, 0, 0, 68, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 3, 0,
	0, 0, 1, 0, 0, 0,
	3, 12, 0, 0, 83, 86,
	95, 80, 79, 83, 73, 84,
	73, 79, 78, 0, 84, 69,
	88, 67, 79, 79, 82, 68,
	0, 171, 171, 171, 83, 72,
	68, 82, 32, 1, 0, 0,
	64, 0, 1, 0, 72, 0,
	0, 0, 96, 0, 0, 4,
	18, 16, 16, 0, 0, 0,
	0, 0, 6, 0, 0, 0,
	103, 0, 0, 4, 242, 32,
	16, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 101, 0,
	0, 3, 50, 32, 16, 0,
	1, 0, 0, 0, 104, 0,
	0, 2, 1, 0, 0, 0,
	54, 0, 0, 8, 194, 32,
	16, 0, 0, 0, 0, 0,
	2, 64, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	128, 63, 1, 0, 0, 7,
	18, 0, 16, 0, 0, 0,
	0, 0, 10, 16, 16, 0,
	0, 0, 0, 0, 1, 64,
	0, 0, 1, 0, 0, 0,
	85, 0, 0, 7, 66, 0,
	16, 0, 0, 0, 0, 0,
	10, 16, 16, 0, 0, 0,
	0, 0, 1, 64, 0, 0,
	1, 0, 0, 0, 86, 0,
	0, 5, 50, 0, 16, 0,
	0, 0, 0, 0, 134, 0,
	16, 0, 0, 0, 0, 0,
	0, 0, 0, 10, 194, 0,
	16, 0, 0, 0, 0, 0,
	6, 4, 16, 0, 0, 0,
	0, 0, 2, 64, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 191,
	0, 0, 0, 191, 54, 0,
	0, 5, 50, 32, 16, 0,
	1, 0, 0, 0, 70, 0,
	16, 0, 0, 0, 0, 0,
	0, 0, 0, 7, 18, 32,
	16, 0, 0, 0, 0, 0,
	42, 0, 16, 0, 0, 0,
	0, 0, 42, 0, 16, 0,
	0, 0, 0, 0, 56, 0,
	0, 7, 34, 32, 16, 0,
	0, 0, 0, 0, 58, 0,
	16, 0, 0, 0, 0, 0,
	1, 64, 0, 0, 0, 0,
	0, 192, 62, 0, 0, 1,
	83, 84, 65, 84, 116, 0,
	0, 0, 9, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 3, 0, 0, 0,
	3, 0, 0, 0, 0, 0,
	0, 0, 2, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	2, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0
};

#include <dwmapi.h>
#include <mmsystem.h>
#include <setupapi.h>

static const GUID GUID_DEVINTERFACE_DISPLAY_ADAPTER = { 0x5b45201d, 0xf2f2, 0x4f3b, { 0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x35, 0x99 } };

#pragma comment(lib, "dwmapi")

static bool IsSafeGPUDriver()
{
	static auto hSetupAPI = LoadLibraryW(L"setupapi.dll");
	if (!hSetupAPI)
	{
		return false;
	}

	static auto _SetupDiGetClassDevsW = (decltype(&SetupDiGetClassDevsW))GetProcAddress(hSetupAPI, "SetupDiGetClassDevsW");
	static auto _SetupDiBuildDriverInfoList = (decltype(&SetupDiBuildDriverInfoList))GetProcAddress(hSetupAPI, "SetupDiBuildDriverInfoList");
	static auto _SetupDiEnumDeviceInfo = (decltype(&SetupDiEnumDeviceInfo))GetProcAddress(hSetupAPI, "SetupDiEnumDeviceInfo");
	static auto _SetupDiEnumDriverInfoW = (decltype(&SetupDiEnumDriverInfoW))GetProcAddress(hSetupAPI, "SetupDiEnumDriverInfoW");
	static auto _SetupDiDestroyDeviceInfoList = (decltype(&SetupDiDestroyDeviceInfoList))GetProcAddress(hSetupAPI, "SetupDiDestroyDeviceInfoList");

	HDEVINFO devInfoSet = _SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISPLAY_ADAPTER, NULL, NULL,
	DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	bool safe = true;

	for (int i = 0;; i++)
	{
		SP_DEVINFO_DATA devInfo = { sizeof(SP_DEVINFO_DATA) };
		if (!_SetupDiEnumDeviceInfo(devInfoSet, i, &devInfo))
		{
			break;
		}

		if (!_SetupDiBuildDriverInfoList(devInfoSet, &devInfo, SPDIT_COMPATDRIVER))
		{
			safe = false;
			break;
		}

		SP_DRVINFO_DATA drvInfo = { sizeof(SP_DRVINFO_DATA) };
		if (_SetupDiEnumDriverInfoW(devInfoSet, &devInfo, SPDIT_COMPATDRIVER, 0, &drvInfo))
		{
			ULARGE_INTEGER driverDate = {0};
			driverDate.HighPart = drvInfo.DriverDate.dwHighDateTime;
			driverDate.LowPart = drvInfo.DriverDate.dwLowDateTime;
			
			// drivers from after 2007-01-01 (to prevent in-box driver from being wrong) and 2020-01-01 are 'unsafe' and might crash
			if (driverDate.QuadPart >= 128120832000000000ULL && driverDate.QuadPart < 132223104000000000ULL)
			{
				safe = false;
				break;
			}
		}
	}

	_SetupDiDestroyDeviceInfoList(devInfoSet);

	return safe;
}

static void InitializeRenderOverlay(winrt::Windows::UI::Xaml::Controls::SwapChainPanel swapChainPanel, int w, int h)
{
	auto nativePanel = swapChainPanel.as<ISwapChainPanelNative>();

	auto run = [w, h, swapChainPanel, nativePanel]()
	{
		auto loadSystemDll = [](auto dll)
		{
			wchar_t systemPath[512];
			GetSystemDirectory(systemPath, _countof(systemPath));

			wcscat_s(systemPath, dll);

			return LoadLibrary(systemPath);
		};

		ComPtr<ID3D11Device> g_pd3dDevice = NULL;
		ComPtr<ID3D11DeviceContext> g_pd3dDeviceContext = NULL;
		ComPtr<IDXGISwapChain1> g_pSwapChain = NULL;
		ComPtr<ID3D11RenderTargetView> g_mainRenderTargetView = NULL;

		// Setup swap chain
		DXGI_SWAP_CHAIN_DESC1 sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 2;
		sd.Width = w;
		sd.Height = h;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

		UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
		D3D_FEATURE_LEVEL featureLevel;

		// so we'll fail if FL11 isn't supported
		const D3D_FEATURE_LEVEL featureLevelArray[1] = {
			D3D_FEATURE_LEVEL_11_0,
		};

		auto d3d11 = loadSystemDll(L"\\d3d11.dll");
		auto _D3D11CreateDevice = (decltype(&D3D11CreateDevice))GetProcAddress(d3d11, "D3D11CreateDevice");

		if (_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, std::size(featureLevelArray), D3D11_SDK_VERSION, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
		{
			return;
		}

		ComPtr<IDXGIDevice1> device1;
		if (FAILED(g_pd3dDevice.As(&device1)))
		{
			return;
		}

		ComPtr<IDXGIAdapter> adapter;
		if (FAILED(device1->GetAdapter(&adapter)))
		{
			return;
		}

		ComPtr<IDXGIFactory> parent;
		if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory), &parent)))
		{
			return;
		}

		ComPtr<IDXGIFactory3> factory3;
		if (FAILED(parent.As(&factory3)))
		{
			return;
		}

		if (FAILED(factory3->CreateSwapChainForComposition(g_pd3dDevice.Get(), &sd, NULL, &g_pSwapChain)))
		{
			return;
		}

		{
			ComPtr<ID3D11Texture2D> pBackBuffer;
			g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
			g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), NULL, &g_mainRenderTargetView);
		}

		swapChainPanel.Dispatcher().TryRunAsync(
		winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
		[g_pSwapChain, nativePanel]()
		{
			nativePanel->SetSwapChain(g_pSwapChain.Get());
		});

		ComPtr<ID3D11VertexShader> vs;
		ComPtr<ID3D11PixelShader> ps;

		g_pd3dDevice->CreatePixelShader(g_PixyShader, sizeof(g_PixyShader), NULL, &ps);
		g_pd3dDevice->CreateVertexShader(g_VertyShader, sizeof(g_VertyShader), NULL, &vs);

		ComPtr<ID3D11BlendState> bs;

		{
			D3D11_BLEND_DESC desc = { 0 };
			desc.AlphaToCoverageEnable = false;
			desc.RenderTarget[0].BlendEnable = true;
			desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

			g_pd3dDevice->CreateBlendState(&desc, &bs);
		}

		struct CBuf
		{
			float res[2];
			float sec;
			float pad;
		};

		ComPtr<ID3D11Buffer> cbuf;

		{
			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = sizeof(CBuf);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			g_pd3dDevice->CreateBuffer(&desc, NULL, &cbuf);
		}

		while (g_uui.ten)
		{
			// Setup viewport
			D3D11_VIEWPORT vp;
			memset(&vp, 0, sizeof(D3D11_VIEWPORT));
			vp.Width = w;
			vp.Height = h;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			vp.TopLeftX = vp.TopLeftY = 0;
			g_pd3dDeviceContext->RSSetViewports(1, &vp);

			auto rtv = g_mainRenderTargetView.Get();
			g_pd3dDeviceContext->OMSetRenderTargets(1, &rtv, NULL);

			float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			g_pd3dDeviceContext->ClearRenderTargetView(rtv, clearColor);

			g_pd3dDeviceContext->VSSetShader(vs.Get(), NULL, 0);
			g_pd3dDeviceContext->PSSetShader(ps.Get(), NULL, 0);

			auto cb = cbuf.Get();
			g_pd3dDeviceContext->VSSetConstantBuffers(0, 1, &cb);
			g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, &cb);

			g_pd3dDeviceContext->OMSetBlendState(bs.Get(), NULL, 0xFFFFFFFF);

			static auto startTime = timeGetTime();

			D3D11_MAPPED_SUBRESOURCE mapped_resource;
			if (SUCCEEDED(g_pd3dDeviceContext->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource)))
			{
				auto c = (CBuf*)mapped_resource.pData;
				c->res[0] = float(w);
				c->res[1] = float(h);
				c->sec = (timeGetTime() - startTime) / 1000.0f;
				g_pd3dDeviceContext->Unmap(cb, 0);
			}

			g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			g_pd3dDeviceContext->Draw(4, 0);

			g_pSwapChain->Present(0, 0);
			DwmFlush();
		}
	};

	std::thread([run]()
	{
		if (IsSafeGPUDriver())
		{
			run();
		}

		// prevent the thread from exiting (the CRT is broken and will crash on thread exit in some cases)
		WaitForSingleObject(GetCurrentProcess(), INFINITE);
	}).detach();
}

void UI_CreateWindow()
{
	g_uui.taskbarMsg = RegisterWindowMessage(L"TaskbarButtonCreated");

	HWND rootWindow = CreateWindowEx(0, L"NotSteamAtAll", PRODUCT_NAME, 13238272 /* lol */, 0x80000000, 0, g_dpi.ScaleX(500), g_dpi.ScaleY(129), NULL, NULL, GetModuleHandle(NULL), 0);

	int wwidth = 500;
	int wheight = 139;

	if (!g_uui.tenMode)
	{
		INITCOMMONCONTROLSEX controlSex;
		controlSex.dwSize = sizeof(controlSex);
		controlSex.dwICC = 16416; // lazy bum
		InitCommonControlsEx(&controlSex);

		HFONT font = UI_CreateScaledFont(-12, 0, 0, 0, 0, 0, 0, 0, 1, 8, 0, 5, 2, L"Tahoma");

		// TODO: figure out which static is placed where
		HWND static1 = CreateWindowEx(0x20, L"static", L"static1", 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(15), g_dpi.ScaleX(455), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL) /* what?! */, 0);

		SendMessage(static1, WM_SETFONT, (WPARAM)font, 0);

		HWND cancelButton = CreateWindowEx(0, L"button", L"Cancel", 0x50000000, g_dpi.ScaleX(395), g_dpi.ScaleY(64), g_dpi.ScaleX(75), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL), 0);
		SendMessage(cancelButton, WM_SETFONT, (WPARAM)font, 0);

		HWND progressBar = CreateWindowEx(0, L"msctls_progress32", 0, 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(40), g_dpi.ScaleX(455), g_dpi.ScaleY(15), rootWindow, 0, GetModuleHandle(NULL), 0);
		SendMessage(progressBar, PBM_SETRANGE32, 0, 10000);

		HWND static2 = CreateWindowEx(0x20, L"static", L"static2", 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(68), g_dpi.ScaleX(370), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL) /* what?! */, 0);
		SendMessage(static2, WM_SETFONT, (WPARAM)font, 0);

		g_uui.cancelButton = cancelButton;
		g_uui.progressBar = progressBar;
		g_uui.topStatic = static1;
		g_uui.bottomStatic = static2;
	}
	else
	{
		wwidth = 525;
		wheight = 525;

		// make TenUI
		auto ten = std::make_unique<TenUI>();
		ten->uiSource = std::move(DesktopWindowXamlSource{});

		// attach window
		auto interop = ten->uiSource.as<IDesktopWindowXamlSourceNative>();
		winrt::check_hresult(interop->AttachToWindow(rootWindow));

		// setup position
		HWND childHwnd;
		interop->get_WindowHandle(&childHwnd);

		SetWindowLong(childHwnd, GWL_EXSTYLE, GetWindowLong(childHwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);

		SetWindowPos(childHwnd, 0, 0, 0, g_dpi.ScaleX(wwidth), g_dpi.ScaleY(wheight), SWP_SHOWWINDOW);

		auto doc = winrt::Windows::UI::Xaml::Markup::XamlReader::Load(g_mainXaml);
		auto ui = doc.as<winrt::Windows::UI::Xaml::FrameworkElement>();

		auto bg = ui.FindName(L"BackdropGrid").as<winrt::Windows::UI::Xaml::Controls::Grid>();
		bg.Background(winrt::make<BackdropBrush>());

		{
			auto sc = ui.FindName(L"Overlay").as<winrt::Windows::UI::Xaml::Controls::SwapChainPanel>();

			auto time = std::time(nullptr);
			auto datetime = std::localtime(&time);
			auto month = datetime->tm_mon + 1;

			// Snow effect for December and January
			if (month == 12 || month == 1)
			{
				InitializeRenderOverlay(sc, g_dpi.ScaleX(wwidth), g_dpi.ScaleY(wheight));
			}
		}

		/*auto shadow = ui.FindName(L"SharedShadow").as<winrt::Windows::UI::Xaml::Media::ThemeShadow>();
		shadow.Receivers().Append(bg);*/

		ten->topStatic = ui.FindName(L"static1").as<winrt::Windows::UI::Xaml::Controls::TextBlock>();
		ten->bottomStatic = ui.FindName(L"static2").as<winrt::Windows::UI::Xaml::Controls::TextBlock>();
		ten->progressBar = ui.FindName(L"progressBar").as<winrt::Windows::UI::Xaml::Controls::ProgressBar>();
		ten->snailContainer = ui.FindName(L"snailContainer").as<winrt::Windows::UI::Xaml::UIElement>();

		ten->uiSource.Content(ui);

		g_uui.tenWindow = FindWindowExW(rootWindow, NULL, L"Windows.UI.Core.CoreWindow", NULL);

		g_uui.ten = std::move(ten);
	}

	g_uui.rootWindow = rootWindow;

	RECT wndRect;
	wndRect.left = 0;
	wndRect.top = 0;
	wndRect.right = g_dpi.ScaleX(wwidth);
	wndRect.bottom = g_dpi.ScaleY(wheight);

	HWND desktop = GetDesktopWindow();
	HDC dc = GetDC(desktop);
	int width = GetDeviceCaps(dc, 8);
	int height = GetDeviceCaps(dc, 10);

	ReleaseDC(desktop, dc);

	SetTimer(rootWindow, 0, 20, NULL);

	MoveWindow(rootWindow, (width - g_dpi.ScaleX(wwidth)) / 2, (height - g_dpi.ScaleY(wheight)) / 2, wndRect.right - wndRect.left, wndRect.bottom - wndRect.top, TRUE);

	ShowWindow(rootWindow, TRUE);
}

LRESULT CALLBACK UI_WndProc(HWND hWnd, UINT uMsg, WPARAM wparam, LPARAM lparam)
{
	switch (uMsg)
	{
		case WM_NCHITTEST:
			if (g_uui.tenMode)
			{
				return HTCAPTION;
			}
		case WM_NCCALCSIZE:
			if (g_uui.tenMode)
			{
				return 0;
			}
		case WM_NCCREATE:
			{
				// Only Windows 10+ supports EnableNonClientDpiScaling
				if (IsWindows10OrGreater())
				{
					HMODULE user32 = LoadLibrary(L"user32.dll");

					if (user32)
					{
						auto EnableNonClientDpiScaling = (decltype(&::EnableNonClientDpiScaling))GetProcAddress(user32, "EnableNonClientDpiScaling");

						if (EnableNonClientDpiScaling)
						{
							EnableNonClientDpiScaling(hWnd);
						}

						FreeLibrary(user32);
					}
				}

				return DefWindowProc(hWnd, uMsg, wparam, lparam);
			}
		
		case WM_CTLCOLORSTATIC:
			SetBkMode((HDC)wparam, TRANSPARENT);
			SetTextColor((HDC)wparam, COLORREF(GetSysColor(COLOR_WINDOWTEXT)));

			return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
		case WM_COMMAND:
			if ((HWND)lparam == g_uui.cancelButton)
			{
				g_uui.canceled = true;
			}

			break;
		case WM_TIMER:
			SetWindowText(g_uui.topStatic, g_uui.topText);
			SetWindowText(g_uui.bottomStatic, g_uui.bottomText);
			break;
		case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC dc = BeginPaint(hWnd, &ps);
			
				EndPaint(hWnd, &ps);
				break;
			}
		case WM_DPICHANGED:
			{
				// Set the new DPI
				g_dpi.SetScale(LOWORD(wparam), HIWORD(wparam));

				// Resize the window
				LPRECT newScale = (LPRECT)lparam;
				SetWindowPos(hWnd, HWND_TOP, newScale->left, newScale->top, newScale->right - newScale->left, newScale->bottom - newScale->top, SWP_NOZORDER | SWP_NOACTIVATE);

				// Recreate the font
				HFONT newFont = UI_CreateScaledFont(-12, 0, 0, 0, 0, 0, 0, 0, 1, 8, 0, 5, 2, L"Tahoma");

				// Resize all components
				SetWindowPos(g_uui.topStatic, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(15), g_dpi.ScaleX(455), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.topStatic, WM_SETFONT, (WPARAM)newFont, 0);

				SetWindowPos(g_uui.cancelButton, HWND_TOP, g_dpi.ScaleX(395), g_dpi.ScaleY(64), g_dpi.ScaleX(75), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.cancelButton, WM_SETFONT, (WPARAM)newFont, 0);

				SetWindowPos(g_uui.progressBar, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(40), g_dpi.ScaleX(455), g_dpi.ScaleY(15), SWP_SHOWWINDOW);

				SetWindowPos(g_uui.bottomStatic, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(68), g_dpi.ScaleX(370), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.bottomStatic, WM_SETFONT, (WPARAM)newFont, 0);
				break;
			}
		case WM_CLOSE:
			g_uui.canceled = true;
			return 0;
		default:
			if (uMsg == g_uui.taskbarMsg)
			{
				if (g_uui.tbList)
				{
					g_uui.tbList->SetProgressState(hWnd, TBPF_NORMAL);
					g_uui.tbList->SetProgressValue(hWnd, 0, 100);
				}
			}
			break;
	}

	return DefWindowProc(hWnd, uMsg, wparam, lparam);
}

void UI_RegisterClass()
{
	WNDCLASSEX wndClass = { 0 };
	wndClass.cbSize = sizeof(wndClass);
	wndClass.style = 3;
	wndClass.lpfnWndProc = UI_WndProc;
	wndClass.cbClsExtra = 0;
	wndClass.cbWndExtra = 0;
	wndClass.hInstance = GetModuleHandle(NULL);
	wndClass.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
	wndClass.hCursor = LoadCursor(NULL, (LPCWSTR)0x7F02);
	wndClass.hbrBackground = (HBRUSH)6;
	wndClass.lpszClassName = L"NotSteamAtAll";
	wndClass.hIconSm = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));

	RegisterClassEx(&wndClass);
}

struct TenUIStorage;

static TenUIStorage* g_tenUI;

struct TenUIStorage : public TenUIBase
{
	// convoluted stuff to prevent WindowsXamlManager destruction weirdness
	static inline thread_local WindowsXamlManager* gManager{ nullptr };

	TenUIStorage()
	{
		g_tenUI = this;
	}

	void InitManager()
	{
		if (!gManager)
		{
			static thread_local WindowsXamlManager manager = WindowsXamlManager::InitializeForCurrentThread();
			gManager = &manager;
		}
	}

	virtual ~TenUIStorage() override
	{
		ShowWindow(g_uui.tenWindow, SW_HIDE);

		g_tenUI = nullptr;
	}

	static void ReallyBreakIt()
	{
		if (gManager)
		{
			gManager->Close();
		}
	}
};

std::unique_ptr<TenUIBase> UI_InitTen()
{
	// Windows 10 RS5+ gets a neat UI
	DWORDLONG viMask = 0;
	OSVERSIONINFOEXW osvi = { 0 };
	osvi.dwOSVersionInfoSize = sizeof(osvi);
	osvi.dwBuildNumber = 17763; // RS5+

	VER_SET_CONDITION(viMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

	bool forceOff = false;

	static HostSharedData<CfxState> initState("CfxInitState");

	if (initState->isReverseGame)
	{
		forceOff = true;
	}

	if (getenv("CitizenFX_NoTenUI"))
	{
		forceOff = true;
	}

#ifdef IS_LAUNCHER
	forceOff = true;
#endif

	if (VerifyVersionInfoW(&osvi, VER_BUILDNUMBER, viMask) && !forceOff)
	{
		RO_REGISTRATION_COOKIE cookie;

		g_uui.tenMode = true;

		try
		{
			return std::make_unique<TenUIStorage>();
		}
		catch (const std::exception&)
		{
		}
	}

	return std::make_unique<TenUIBase>();
}

void UI_DestroyTen()
{
	TenUIStorage::ReallyBreakIt();
}

void UI_DoCreation(bool safeMode)
{
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	if (g_tenUI)
	{
		g_tenUI->InitManager();
	}

	if (IsWindows7OrGreater())
	{
		CoCreateInstance(CLSID_TaskbarList, 
			NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uui.tbList));
	}

	// Only Windows 8.1+ supports per-monitor DPI awareness
	if (IsWindows8Point1OrGreater())
	{
		HMODULE shCore = LoadLibrary(L"shcore.dll");

		if (shCore)
		{
			auto GetDpiForMonitor = (decltype(&::GetDpiForMonitor))GetProcAddress(shCore, "GetDpiForMonitor");

			if (GetDpiForMonitor)
			{
				UINT dpiX, dpiY;

				POINT point;
				point.x = 1;
				point.y = 1;

				// Get DPI for the main monitor
				HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
				GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
				g_dpi.SetScale(dpiX, dpiY);
			}

			FreeLibrary(shCore);
		}
	}

	static bool lastTen = g_uui.tenMode;

	if (safeMode)
	{
		lastTen = g_uui.tenMode;
		g_uui.tenMode = false;
	}
	else
	{
		g_uui.tenMode = lastTen;
	}

	UI_RegisterClass();
	UI_CreateWindow();
}

void UI_DoDestruction()
{
	static HostSharedData<CfxState> initState("CfxInitState");
	AllowSetForegroundWindow((initState->gamePid) ? initState->gamePid : GetCurrentProcessId());

	ShowWindow(g_uui.rootWindow, SW_HIDE);

	g_uui.ten = {};

	DestroyWindow(g_uui.rootWindow);
}

void UI_SetSnailState(bool snail)
{
	if (g_uui.ten)
	{
		g_uui.ten->snailContainer.Visibility(snail ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed);

		return;
	}
}

void UI_UpdateText(int textControl, const wchar_t* text)
{
	if (g_uui.ten)
	{
		std::wstring tstr = text;

		if (textControl == 0)
		{
			g_uui.ten->topStatic.Text(tstr);
		}
		else
		{
			g_uui.ten->bottomStatic.Text(tstr);
		}

		return;
	}

	if (textControl == 0)
	{
		wcscpy(g_uui.topText, text);
	}
	else
	{
		wcscpy(g_uui.bottomText, text);
	}
}

void UI_UpdateProgress(double percentage)
{
	if (g_uui.ten)
	{
		try
		{
			g_uui.ten->progressBar.Maximum(100.0);
			g_uui.ten->progressBar.Value(percentage);
		}
		catch (...)
		{
		}

		g_uui.ten->progressBar.IsIndeterminate(percentage == 100);

		return;
	}

	SendMessage(g_uui.progressBar, PBM_SETPOS, (int)(percentage * 100), 0);

	if (g_uui.tbList)
	{
		g_uui.tbList->SetProgressValue(g_uui.rootWindow, (int)percentage, 100);

		if (percentage == 100)
		{
			g_uui.tbList->SetProgressState(g_uui.rootWindow, TBPF_NOPROGRESS);
		}
	}
}

bool UI_IsCanceled()
{
	return g_uui.canceled;
}

void UI_DisplayError(const wchar_t* error)
{
	static TASKDIALOGCONFIG taskDialogConfig = { 0 };
	taskDialogConfig.cbSize = sizeof(taskDialogConfig);
	taskDialogConfig.hInstance = GetModuleHandle(nullptr);
	taskDialogConfig.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT;
	taskDialogConfig.dwCommonButtons = TDCBF_CLOSE_BUTTON;
	taskDialogConfig.pszWindowTitle = L"Error updating " PRODUCT_NAME;
	taskDialogConfig.pszMainIcon = TD_ERROR_ICON;
	taskDialogConfig.pszMainInstruction = NULL;
	taskDialogConfig.pszContent = error;

	TaskDialogIndirect(&taskDialogConfig, nullptr, nullptr, nullptr);
}

#include <wrl/module.h>

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
#ifdef _WRL_MODULE_H_
	if (!::Microsoft::WRL::Module<::Microsoft::WRL::InProc>::GetModule().Terminate())
	{
		return 1; // S_FALSE
	}
#endif

	if (winrt::get_module_lock())
	{
		return 1; // S_FALSE
	}

	winrt::clear_factory_cache();
	return 0; // S_OK
}

extern "C" DLL_EXPORT HRESULT WINAPI DllGetActivationFactory(HSTRING classId, IActivationFactory** factory)
{
	try
	{
		*factory = nullptr;
		uint32_t length{};
		wchar_t const* const buffer = WindowsGetStringRawBuffer(classId, &length);
		std::wstring_view const name{ buffer, length };

		auto requal = [](std::wstring_view const& left, std::wstring_view const& right) noexcept
		{
			return std::equal(left.rbegin(), left.rend(), right.rbegin(), right.rend());
		};

		if (requal(name, L"CitiLaunch.BackdropBrush"))
		{
			*factory = (IActivationFactory*)winrt::detach_abi(winrt::make<BackdropBrush>());
			return 0;
		}

#ifdef _WRL_MODULE_H_
		return ::Microsoft::WRL::Module<::Microsoft::WRL::InProc>::GetModule().GetActivationFactory(static_cast<HSTRING>(classId), reinterpret_cast<::IActivationFactory * *>(factory));
#else
		return winrt::hresult_class_not_available(name).to_abi();
#endif
	}
	catch (...) { return winrt::to_hresult(); }
}

#endif
