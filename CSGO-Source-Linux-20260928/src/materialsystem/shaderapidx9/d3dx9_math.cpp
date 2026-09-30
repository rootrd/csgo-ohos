// D3DX math used by the native D3D9 renderer. D3DX matrices use row vectors;
// translation is in row 4 and Local operations multiply on the left.
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include "dxvk/d3dx9_compat.h"

D3DXMATRIX *WINAPI D3DXMatrixMultiply( D3DXMATRIX *out, const D3DXMATRIX *a, const D3DXMATRIX *b )
{
    D3DXMATRIX result;
    for ( int i = 0; i < 4; ++i )
        for ( int j = 0; j < 4; ++j )
            result.m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j]
                           + a->m[i][2]*b->m[2][j] + a->m[i][3]*b->m[3][j];
    *out = result;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixTranspose( D3DXMATRIX *out, const D3DXMATRIX *in )
{
    D3DXMATRIX result;
    for ( int i = 0; i < 4; ++i )
        for ( int j = 0; j < 4; ++j )
            result.m[i][j] = in->m[j][i];
    *out = result;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixInverse( D3DXMATRIX *out, FLOAT *determinant, const D3DXMATRIX *in )
{
    // Cofactors also provide the determinant requested by the shader API.
    // Work in a temporary so in-place inversion and singular inputs are safe.
    D3DXMATRIX adjugate;
    for ( int row = 0; row < 4; ++row )
        for ( int col = 0; col < 4; ++col )
        {
            float minor[9];
            int n = 0;
            for ( int i = 0; i < 4; ++i )
                for ( int j = 0; j < 4; ++j )
                    if ( i != row && j != col ) minor[n++] = in->m[i][j];
            float cofactor = minor[0]*(minor[4]*minor[8] - minor[5]*minor[7])
                           - minor[1]*(minor[3]*minor[8] - minor[5]*minor[6])
                           + minor[2]*(minor[3]*minor[7] - minor[4]*minor[6]);
            adjugate.m[col][row] = (row + col) & 1 ? -cofactor : cofactor;
        }
    float det = in->m[0][0]*adjugate.m[0][0] + in->m[0][1]*adjugate.m[1][0]
              + in->m[0][2]*adjugate.m[2][0] + in->m[0][3]*adjugate.m[3][0];
    if ( determinant ) *determinant = det;
    if ( det == 0.0f ) return nullptr;
    for ( int i = 0; i < 4; ++i )
        for ( int j = 0; j < 4; ++j )
            adjugate.m[i][j] /= det;
    *out = adjugate;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixTranslation( D3DXMATRIX *out, FLOAT x, FLOAT y, FLOAT z )
{
    D3DXMatrixIdentity(out);
    out->_41 = x; out->_42 = y; out->_43 = z;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixScaling( D3DXMATRIX *out, FLOAT x, FLOAT y, FLOAT z )
{
    D3DXMatrixIdentity(out);
    out->_11 = x; out->_22 = y; out->_33 = z;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixRotationAxis( D3DXMATRIX *out, const D3DXVECTOR3 *axis, FLOAT angle )
{
    const float length = std::sqrt(axis->x*axis->x + axis->y*axis->y + axis->z*axis->z);
    const float x = length ? axis->x / length : 0;
    const float y = length ? axis->y / length : 0;
    const float z = length ? axis->z / length : 0;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    D3DXMatrixIdentity(out);
    out->_11 = t*x*x+c;   out->_12 = t*x*y+s*z; out->_13 = t*x*z-s*y;
    out->_21 = t*x*y-s*z; out->_22 = t*y*y+c;   out->_23 = t*y*z+s*x;
    out->_31 = t*x*z+s*y; out->_32 = t*y*z-s*x; out->_33 = t*z*z+c;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixRotationYawPitchRoll( D3DXMATRIX *out, FLOAT yaw, FLOAT pitch, FLOAT roll )
{
    D3DXMATRIX y, p, r, rp;
    const D3DXVECTOR3 yAxis(0,1,0), xAxis(1,0,0), zAxis(0,0,1);
    D3DXMatrixRotationAxis(&y, &yAxis, yaw);
    D3DXMatrixRotationAxis(&p, &xAxis, pitch);
    D3DXMatrixRotationAxis(&r, &zAxis, roll);
    D3DXMatrixMultiply(&rp, &r, &p);
    return D3DXMatrixMultiply(out, &rp, &y);
}

D3DXMATRIX *WINAPI D3DXMatrixOrthoOffCenterRH( D3DXMATRIX *out, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity(out);
    out->_11 = 2.0f / (r-l); out->_22 = 2.0f / (t-b); out->_33 = 1.0f / (zn-zf);
    out->_41 = (l+r) / (l-r); out->_42 = (t+b) / (b-t); out->_43 = zn / (zn-zf);
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveOffCenterRH( D3DXMATRIX *out, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity(out);
    out->_11 = 2.0f*zn / (r-l); out->_22 = 2.0f*zn / (t-b);
    out->_31 = (l+r) / (r-l); out->_32 = (t+b) / (t-b);
    out->_33 = zf / (zn-zf); out->_34 = -1.0f;
    out->_43 = zn*zf / (zn-zf); out->_44 = 0.0f;
    return out;
}

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveRH( D3DXMATRIX *out, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
    return D3DXMatrixPerspectiveOffCenterRH(out, -w*0.5f, w*0.5f, -h*0.5f, h*0.5f, zn, zf);
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform( D3DXVECTOR4 *out, const D3DXVECTOR4 *in, const D3DXMATRIX *m )
{
    D3DXVECTOR4 result;
    result.x = in->x*m->_11 + in->y*m->_21 + in->z*m->_31 + in->w*m->_41;
    result.y = in->x*m->_12 + in->y*m->_22 + in->z*m->_32 + in->w*m->_42;
    result.z = in->x*m->_13 + in->y*m->_23 + in->z*m->_33 + in->w*m->_43;
    result.w = in->x*m->_14 + in->y*m->_24 + in->z*m->_34 + in->w*m->_44;
    *out = result;
    return out;
}

D3DXVECTOR3 *WINAPI D3DXVec3TransformCoord( D3DXVECTOR3 *out, const D3DXVECTOR3 *in, const D3DXMATRIX *m )
{
    D3DXVECTOR4 v(in->x, in->y, in->z, 1.0f);
    D3DXVec4Transform(&v, &v, m);
    *out = D3DXVECTOR3(v.x/v.w, v.y/v.w, v.z/v.w);
    return out;
}

D3DXVECTOR4 *WINAPI D3DXVec4Normalize( D3DXVECTOR4 *out, const D3DXVECTOR4 *in )
{
    const float length = std::sqrt(in->x*in->x + in->y*in->y + in->z*in->z + in->w*in->w);
    *out = length ? D3DXVECTOR4(in->x/length, in->y/length, in->z/length, in->w/length) : D3DXVECTOR4(0,0,0,0);
    return out;
}

D3DXPLANE *WINAPI D3DXPlaneNormalize( D3DXPLANE *out, const D3DXPLANE *in )
{
    const float length = std::sqrt(in->a*in->a + in->b*in->b + in->c*in->c);
    *out = length ? D3DXPLANE(in->a/length, in->b/length, in->c/length, in->d/length) : D3DXPLANE(0,0,0,0);
    return out;
}

D3DXPLANE *WINAPI D3DXPlaneTransform( D3DXPLANE *out, const D3DXPLANE *in, const D3DXMATRIX *m )
{
    D3DXVECTOR4 v(in->a, in->b, in->c, in->d);
    D3DXVec4Transform(&v, &v, m);
    *out = D3DXPLANE(v.x,v.y,v.z,v.w);
    return out;
}

namespace {
class MatrixStack final : public ID3DXMatrixStack
{
    struct Entry { D3DXMATRIX matrix; Entry *previous; };
    std::atomic<ULONG> m_refs{1};
    Entry m_base;
    Entry *m_top = &m_base;
public:
    MatrixStack() { m_base.previous = nullptr; D3DXMatrixIdentity(&m_base.matrix); }
    ~MatrixStack() { while ( m_top != &m_base ) Pop(); }
    HRESULT STDMETHODCALLTYPE QueryInterface( REFIID iid, void **out ) override
    {
        if ( !out ) return E_POINTER;
        *out = nullptr;
        if ( std::memcmp(&iid, &IID_IUnknown, sizeof(GUID)) && std::memcmp(&iid, &IID_ID3DXMatrixStack, sizeof(GUID)) )
            return E_NOINTERFACE;
        *out = static_cast<ID3DXMatrixStack *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if ( !refs ) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Pop() override
    {
        if ( m_top == &m_base ) return D3DERR_INVALIDCALL;
        Entry *entry = m_top;
        m_top = entry->previous;
        delete entry;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Push() override
    {
        Entry *entry = new (std::nothrow) Entry{m_top->matrix, m_top};
        if ( !entry ) return E_OUTOFMEMORY;
        m_top = entry;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LoadIdentity() override { D3DXMatrixIdentity(GetTop()); return S_OK; }
    HRESULT STDMETHODCALLTYPE LoadMatrix( const D3DXMATRIX *m ) override
    { if ( !m ) return D3DERR_INVALIDCALL; *GetTop() = *m; return S_OK; }
    HRESULT STDMETHODCALLTYPE MultMatrix( const D3DXMATRIX *m ) override
    { if ( !m ) return D3DERR_INVALIDCALL; D3DXMatrixMultiply(GetTop(), GetTop(), m); return S_OK; }
    HRESULT STDMETHODCALLTYPE MultMatrixLocal( const D3DXMATRIX *m ) override
    { if ( !m ) return D3DERR_INVALIDCALL; D3DXMatrixMultiply(GetTop(), m, GetTop()); return S_OK; }
    HRESULT STDMETHODCALLTYPE RotateAxis( const D3DXVECTOR3 *v, FLOAT angle ) override
    { if ( !v ) return D3DERR_INVALIDCALL; D3DXMATRIX m; D3DXMatrixRotationAxis(&m,v,angle); return MultMatrix(&m); }
    HRESULT STDMETHODCALLTYPE RotateAxisLocal( const D3DXVECTOR3 *v, FLOAT angle ) override
    { if ( !v ) return D3DERR_INVALIDCALL; D3DXMATRIX m; D3DXMatrixRotationAxis(&m,v,angle); return MultMatrixLocal(&m); }
    HRESULT STDMETHODCALLTYPE RotateYawPitchRoll( FLOAT yaw, FLOAT pitch, FLOAT roll ) override
    { D3DXMATRIX m; D3DXMatrixRotationYawPitchRoll(&m,yaw,pitch,roll); return MultMatrix(&m); }
    HRESULT STDMETHODCALLTYPE RotateYawPitchRollLocal( FLOAT yaw, FLOAT pitch, FLOAT roll ) override
    { D3DXMATRIX m; D3DXMatrixRotationYawPitchRoll(&m,yaw,pitch,roll); return MultMatrixLocal(&m); }
    HRESULT STDMETHODCALLTYPE Scale( FLOAT x, FLOAT y, FLOAT z ) override
    { D3DXMATRIX m; D3DXMatrixScaling(&m,x,y,z); return MultMatrix(&m); }
    HRESULT STDMETHODCALLTYPE ScaleLocal( FLOAT x, FLOAT y, FLOAT z ) override
    { D3DXMATRIX m; D3DXMatrixScaling(&m,x,y,z); return MultMatrixLocal(&m); }
    HRESULT STDMETHODCALLTYPE Translate( FLOAT x, FLOAT y, FLOAT z ) override
    { D3DXMATRIX m; D3DXMatrixTranslation(&m,x,y,z); return MultMatrix(&m); }
    HRESULT STDMETHODCALLTYPE TranslateLocal( FLOAT x, FLOAT y, FLOAT z ) override
    { D3DXMATRIX m; D3DXMatrixTranslation(&m,x,y,z); return MultMatrixLocal(&m); }
    D3DXMATRIX *STDMETHODCALLTYPE GetTop() override { return &m_top->matrix; }
};
}

HRESULT WINAPI D3DXCreateMatrixStack( DWORD, ID3DXMatrixStack **out )
{
    if ( !out ) return D3DERR_INVALIDCALL;
    *out = new (std::nothrow) MatrixStack;
    return *out ? S_OK : E_OUTOFMEMORY;
}

LPCSTR WINAPI D3DXGetPixelShaderProfile( IDirect3DDevice9 *device )
{
    D3DCAPS9 caps = {};
    if ( !device || FAILED(device->GetDeviceCaps(&caps)) ) return nullptr;
    if ( caps.PixelShaderVersion >= D3DPS_VERSION(3,0) ) return "ps_3_0";
    if ( caps.PixelShaderVersion >= D3DPS_VERSION(2,0) ) return "ps_2_0";
    if ( caps.PixelShaderVersion >= D3DPS_VERSION(1,4) ) return "ps_1_4";
    if ( caps.PixelShaderVersion >= D3DPS_VERSION(1,3) ) return "ps_1_3";
    if ( caps.PixelShaderVersion >= D3DPS_VERSION(1,2) ) return "ps_1_2";
    return caps.PixelShaderVersion >= D3DPS_VERSION(1,1) ? "ps_1_1" : nullptr;
}
