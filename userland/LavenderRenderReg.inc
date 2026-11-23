/* LavenderRenderReg.inc: registers the classes of LavenderRender.inc (included after newClass and ADD) */

static void
registerRenderClasses(Class device)
{
	static const char * const tex[] = { "MTLTexture", "MTLResource", NULL };
	static const char * const rps[] = { "MTLRenderPipelineState", NULL };
	static const char * const smp[] = { "MTLSamplerState", NULL };
	static const char * const rce[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder", NULL };

	if ((gTextureClass = newClass("LavenderMetalTexture", "_MTLTexture")) != Nil) {
		declareNamed(gTextureClass, tex);
		addStubForwarding(gTextureClass);
		ADD(gTextureClass, "dealloc", texDealloc, "v@:");
		ADD(gTextureClass, "device", objDevice, "@@:");
		ADD(gTextureClass, "label", texLabel, "@@:");
		ADD(gTextureClass, "setLabel:", texSetLabel, "v@:@");
		ADD(gTextureClass, "iosurface", texIOSurface, "^{__IOSurface=}@:");
		ADD(gTextureClass, "iosurfacePlane", texOne, "Q@:");
		ADD(gTextureClass, "width", texWidth, "Q@:");
		ADD(gTextureClass, "height", texHeight, "Q@:");
		ADD(gTextureClass, "depth", texOne, "Q@:");
		ADD(gTextureClass, "mipmapLevelCount", texOne, "Q@:");
		ADD(gTextureClass, "sampleCount", texOne, "Q@:");
		ADD(gTextureClass, "arrayLength", texOne, "Q@:");
		ADD(gTextureClass, "textureType", texType, "Q@:");
		ADD(gTextureClass, "pixelFormat", texFormat, "Q@:");
		ADD(gTextureClass, "usage", texUsage, "Q@:");
		ADD(gTextureClass, "storageMode", texStorage, "Q@:");
		ADD(gTextureClass, "allocatedSize", texSize, "Q@:");
		ADD(gTextureClass, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", texGetBytes,
		    "v@:^vQ{_MTLRegion={_MTLOrigin=QQQ}{_MTLSize=QQQ}}Q");
		ADD(gTextureClass, "getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:", texGetBytesSlice,
		    "v@:^vQQ{_MTLRegion={_MTLOrigin=QQQ}{_MTLSize=QQQ}}QQ");
		ADD(gTextureClass, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", texReplace,
		    "v@:{_MTLRegion={_MTLOrigin=QQQ}{_MTLSize=QQQ}}Qr^vQ");
		ADD(gTextureClass, "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", texReplaceSlice,
		    "v@:{_MTLRegion={_MTLOrigin=QQQ}{_MTLSize=QQQ}}QQr^vQQ");
		objc_registerClassPair(gTextureClass);
		ADD(device, "newTextureWithDescriptor:", deviceNewTexture, "@@:@");
		ADD(device, "newTextureWithDescriptor:iosurface:plane:", deviceNewTextureSurface, "@@:@^{__IOSurface=}Q");
	}
	if ((gRenderPipeClass = newClass("LavenderMetalRenderPipeline", "_MTLRenderPipelineState")) != Nil) {
		declareNamed(gRenderPipeClass, rps);
		addStubForwarding(gRenderPipeClass);
		ADD(gRenderPipeClass, "dealloc", rpDealloc, "v@:");
		ADD(gRenderPipeClass, "device", objDevice, "@@:");
		ADD(gRenderPipeClass, "label", rpLabel, "@@:");
		ADD(gRenderPipeClass, "setLabel:", rpSetLabel, "v@:@");
		objc_registerClassPair(gRenderPipeClass);
		ADD(device, "newRenderPipelineStateWithDescriptor:error:", deviceNewRenderPipeline, "@@:@^@");
		ADD(device, "newRenderPipelineStateWithDescriptor:options:reflection:error:", deviceNewRenderPipelineOptions, "@@:@Q^@^@");
		ADD(device, "newRenderPipelineStateWithDescriptor:completionHandler:", deviceNewRenderPipelineAsync, "v@:@@?");
	}
	if ((gSamplerClass = newClass("LavenderMetalSampler", "_MTLSamplerState")) != Nil) {
		declareNamed(gSamplerClass, smp);
		addStubForwarding(gSamplerClass);
		ADD(gSamplerClass, "device", objDevice, "@@:");
		ADD(gSamplerClass, "label", sampLabel, "@@:");
		ADD(gSamplerClass, "setLabel:", sampSetLabel, "v@:@");
		objc_registerClassPair(gSamplerClass);
		ADD(device, "newSamplerStateWithDescriptor:", deviceNewSampler, "@@:@");
	}
	if ((gRenderClass = newClass("LavenderMetalRenderEncoder", "_MTLCommandEncoder")) != Nil) {
		declareNamed(gRenderClass, rce);
		addStubForwarding(gRenderClass);
		ADD(gRenderClass, "dealloc", rencDealloc, "v@:");
		ADD(gRenderClass, "device", objDevice, "@@:");
		ADD(gRenderClass, "label", rencLabel, "@@:");
		ADD(gRenderClass, "setLabel:", rencSetLabel, "v@:@");
		ADD(gRenderClass, "setRenderPipelineState:", rencSetPipeline, "v@:@");
		ADD(gRenderClass, "setVertexBuffer:offset:atIndex:", rencSetVBuffer, "v@:@QQ");
		ADD(gRenderClass, "setVertexBufferOffset:atIndex:", rencSetVOffset, "v@:QQ");
		ADD(gRenderClass, "setVertexBytes:length:atIndex:", rencSetVBytes, "v@:r^vQQ");
		ADD(gRenderClass, "setFragmentBuffer:offset:atIndex:", rencSetFBuffer, "v@:@QQ");
		ADD(gRenderClass, "setFragmentBufferOffset:atIndex:", rencSetFOffset, "v@:QQ");
		ADD(gRenderClass, "setFragmentBytes:length:atIndex:", rencSetFBytes, "v@:r^vQQ");
		ADD(gRenderClass, "setViewport:", rencSetViewport, "v@:{_MTLViewport=dddddd}");
		ADD(gRenderClass, "setScissorRect:", rencSetScissor, "v@:{_MTLScissorRect=QQQQ}");
		ADD(gRenderClass, "setFragmentTexture:atIndex:", rencSetFTexture, "v@:@Q");
		ADD(gRenderClass, "setFragmentTextures:withRange:", rencSetFTextures, "v@:r^@{_NSRange=QQ}");
		ADD(gRenderClass, "setFragmentSamplerState:atIndex:", rencSetFSampler, "v@:@Q");
		ADD(gRenderClass, "setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:", rencSetFSamplerLod, "v@:@ffQ");
		ADD(gRenderClass, "setFragmentSamplerStates:withRange:", rencSetFSamplers, "v@:r^@{_NSRange=QQ}");
		ADD(gRenderClass, "setVertexTexture:atIndex:", rencSetVTexture, "v@:@Q");
		ADD(gRenderClass, "setBlendColorRed:green:blue:alpha:", rencSetBlendColor, "v@:ffff");
		ADD(gRenderClass, "setCullMode:", rencSetCull, "v@:Q");
		ADD(gRenderClass, "setFrontFacingWinding:", rencSetWinding, "v@:Q");
		ADD(gRenderClass, "setTriangleFillMode:", rencIgnore, "v@:Q");
		ADD(gRenderClass, "setDepthClipMode:", rencIgnore, "v@:Q");
		ADD(gRenderClass, "setDepthStencilState:", rencIgnoreObj, "v@:@");
		ADD(gRenderClass, "drawPrimitives:vertexStart:vertexCount:", rencDraw, "v@:QQQ");
		ADD(gRenderClass, "drawPrimitives:vertexStart:vertexCount:instanceCount:", rencDrawInstanced, "v@:QQQQ");
		ADD(gRenderClass, "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", rencDrawBase, "v@:QQQQQ");
		ADD(gRenderClass, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", rencDrawIndexed, "v@:QQQ@Q");
		ADD(gRenderClass, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", rencDrawIndexedInst, "v@:QQQ@QQ");
		ADD(gRenderClass, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:", rencDrawIndexedBase, "v@:QQQ@QQqQ");
		ADD(gRenderClass, "endEncoding", rencEnd, "v@:");
		objc_registerClassPair(gRenderClass);
		ADD(gCommandBufferClass, "renderCommandEncoderWithDescriptor:", cbRenderEncoder, "@@:@");
		ADD(gCommandBufferClass, "presentDrawable:", cbPresent, "v@:@");
		ADD(gCommandBufferClass, "presentDrawable:atTime:", cbPresentAt, "v@:@d");
		ADD(gCommandBufferClass, "presentDrawable:afterMinimumDuration:", cbPresentAt, "v@:@d");
	}
}
