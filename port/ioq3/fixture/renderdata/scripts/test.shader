// shaders sinteticos de teste
textures/test/fence
{
	cull none
	surfaceparm trans
	{
		map textures/test/fence.tga
		alphaFunc GE128
		rgbGen identity
	}
	{
		map $lightmap
		blendFunc filter
		depthFunc equal
	}
}

textures/test/glowpad
{
	surfaceparm nolightmap
	{
		map textures/test/glow.tga
		blendFunc add
		rgbGen identity
		tcMod rotate 40
	}
}

textures/test/lavafloor
{
	{
		map $lightmap
		rgbGen identity
	}
	{
		map textures/test/floor.jpg
		blendFunc filter
		tcMod scroll 0.1 0.05
	}
}
