#ifndef XS_X11_DISPATCH_STUB_H
#define XS_X11_DISPATCH_STUB_H

/*
 * Host/Android compatibility stub.
 *
 * The vendored C touch engine only includes this header from core/tick.c and
 * does not call X11 or JNI symbols from it.  The upstream header pulls in JNI,
 * which is intentionally absent from the detached overlay build.
 */

#endif /* XS_X11_DISPATCH_STUB_H */
