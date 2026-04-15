#include "homography.h"

/*
  Compute homography H mapping unit square (u,v) to quad (x,y).
  Corner mapping order:
    (0,0)->BL, (1,0)->BR, (1,1)->TR, (0,1)->TL
  H row-major:
    [ a b c
      d e f
      g h 1 ]
*/
void homography_square_to_quad(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float x3, float y3,
    float H[9]
) {
    float dx1 = x1 - x2;
    float dx2 = x3 - x2;
    float dx3 = x0 - x1 + x2 - x3;

    float dy1 = y1 - y2;
    float dy2 = y3 - y2;
    float dy3 = y0 - y1 + y2 - y3;

    float a,b,c,d,e,f,g,h;

    if (dx3 == 0.0f && dy3 == 0.0f) {
        a = x1 - x0;  b = x3 - x0;  c = x0;
        d = y1 - y0;  e = y3 - y0;  f = y0;
        g = 0.0f;     h = 0.0f;
    } else {
        float det = dx1 * dy2 - dx2 * dy1;
        if (det == 0.0f) det = 1e-6f;

        g = (dx3 * dy2 - dx2 * dy3) / det;
        h = (dx1 * dy3 - dx3 * dy1) / det;

        a = x1 - x0 + g * x1;
        b = x3 - x0 + h * x3;
        c = x0;

        d = y1 - y0 + g * y1;
        e = y3 - y0 + h * y3;
        f = y0;
    }

    H[0]=a; H[1]=b; H[2]=c;
    H[3]=d; H[4]=e; H[5]=f;
    H[6]=g; H[7]=h; H[8]=1.0f;
}

void apply_homography(const float H[9], float u, float v, float* x, float* y)
{
    float a=H[0], b=H[1], c=H[2];
    float d=H[3], e=H[4], f=H[5];
    float g=H[6], h=H[7];

    float denom = g*u + h*v + 1.0f;
    if (denom == 0.0f) denom = 1e-6f;

    *x = (a*u + b*v + c) / denom;
    *y = (d*u + e*v + f) / denom;
}
