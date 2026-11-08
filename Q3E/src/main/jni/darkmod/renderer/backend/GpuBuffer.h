/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#pragma once

/**
 * Management class for a universal GPU buffer containing dynamic data that is
 * regularly updated every frame. Can be used for vertex/index data, UBOs,
 * multidraw commands, ...
 *
 * The buffer is divided into three frames - one frame for the GPU to draw with,
 * one for the CPU to write to and one "in-transit" for the GL driver, so that
 * we avoid synchronization between CPU and GPU in all but the rarest of cases.
 *
 * To make use of the buffer, get its current write address with `CurrentWriteLocation`
 * and the `BytesRemaining` in the current frame region and copy your data to it.
 * Once you are done writing your data and before using it in a GPU operation, you
 * need to `Commit` the range you wrote to. Bind the buffer or the write location's
 * range as needed.
 *
 * After a frame is completed, you need to call `SwitchFrame` to switch to the next
 * frame region and issue sync fences and waits to make sure no buffer region is
 * written to that is still in use by the GPU.
 */
class GpuBuffer {
public:
	void Init( GLenum type, GLuint size, GLuint alignment, bool retainPreviousFrame = false, bool isolateFrames = false );

	/// Use this variant when the CPU prepares data one frame ahead (e.g. VertexCache)
	/// Otherwise, it is assumed that GPU draw calls are issued from the same region
	/// that was written to in this frame. That's the default for buffers filled and used
	/// in the backend.
	void InitWriteFrameAhead( GLenum type, GLuint size, GLuint alignment, bool retainPreviousFrame = false, bool isolateFrames = false );
	void Destroy();

	byte *CurrentWriteLocation() const;
	GLuint BytesRemaining() const;

	void Commit( GLuint numBytes );
	
	void BindRangeToIndexTarget( GLuint index, byte *offset, GLuint size );
	void Bind();
	const void * BufferOffset( const void *pointer );

	void SwitchFrame();

	GLuint GetAPIObject() const { return isolatedFrames ? frameBuffers[currentDrawingFrame] : bufferObject; }
	GLuint GetAPIObjectForOffset(GLuint offset) const {
		if (!isolatedFrames) return bufferObject;
		assert(frameSize && offset < totalSize);
		return frameBuffers[offset / frameSize];
	}
	GLuint PhysicalOffset(GLuint offset) const { return isolatedFrames ? offset % frameSize : offset; }
	bool HasIsolatedFrames() const { return isolatedFrames; }
	int FrameCount() const { return numFrames; }
	// Backend-only access to the existing CPU staging copy, never GPU mapping
	// or readback. The frontend writes a different ring region. Persistent
	// mappings were created write-only and must not be read through this API.
	const byte *ReadDrawingRange(GLuint offset, GLuint bytes) const {
		if (usesPersistentMapping || !bufferContents || !frameSize || !bytes) return nullptr;
		const GLuint begin = currentDrawingFrame * frameSize;
		if (offset < begin || offset - begin >= frameSize || bytes > frameSize - (offset - begin)) return nullptr;
		return bufferContents + offset;
	}

	static const int NUM_FRAMES = 3;

private:
	GLsync frameFences[NUM_FRAMES + 1] = { nullptr };
	int numFrames = NUM_FRAMES;
	bool retainPreviousFrame = false;
	GLenum type = GL_INVALID_ENUM;
	GLuint frameSize = 0;
	GLuint totalSize = 0;
	GLuint alignment = 0;
	bool usesPersistentMapping = false;
	GLuint bufferObject = 0;
	GLuint frameBuffers[NUM_FRAMES + 1] = {};
	bool isolatedFrames = false;
	byte *bufferContents = nullptr;
	int currentDrawingFrame = 0;
	int currentWritingFrame = 0;
	GLuint bytesCommittedInCurrentFrame = 0;

	GLuint CurrentOffset() const;
};
