/* =========================================================================
 * TYPE_TY AVAS : L1 line-model coefficient table  (GENERATED -- DO NOT EDIT)
 *
 * GENERATED, and the generator is not part of this repository: this table is the
 * shipped artifact, not something to hand-edit or re-derive here.
 *
 * Model:  y(t) = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])
 *
 * 185 lines, 112.7 .. 2914.6 Hz.  These are NOT a harmonic grid: most of them sit
 * off any multiple of the lowest component.  One perceived 'harmonic' is a group
 * of 6-13 lines within +-10 Hz, and their mutual beating is the richness a grid
 * model cannot produce.
 *
 * ORDER: frequency ascending.  Each cluster below is a contiguous run of
 * entries, so the firmware needs no per-line cluster index.
 *
 * CLUSTER DECOMPOSITION  (max span 200 Hz -> 11 clusters)
 * Per cluster, exactly:
 *     sum_j A_j cos(2 pi f_j t + p_j) = Re{ e^{i 2 pi fc t} * Z(t) },
 *     Z(t) = sum_j A_j e^{i (2 pi (f_j - fc) t + p_j)}
 * Z is band-limited to the cluster half-span, so the firmware rebuilds it
 * once every AVAS_TYPE_TY_DEC samples and linearly interpolates between rebuilds,
 * while only the 11 carriers run at 48 kHz.  All 185 lines stay alive.
 * fc is the amplitude-weighted centroid (smallest baseband offset on the
 * strongest lines = smallest interpolation error).
 *
 *   #   carrier Hz    lines   span Hz   max |f-fc|   energy %
 *   0      185.99       25    187.64      114.36       0.47
 *   1      456.05       27    191.62      133.24      15.91
 *   2      683.27       27    199.04      158.88      72.06
 *   3      786.19       19    179.69      118.72       7.76
 *   4     1069.76       13    188.19      103.69       0.19
 *   5     1249.52       19    163.71       83.78       0.31
 *   6     1477.63       28    199.70      102.80       2.71
 *   7     1652.06       10    128.38       71.88       0.25
 *   8     1977.90        9    185.42      144.60       0.17
 *   9     2167.53        6     40.58       28.76       0.14
 *  10     2909.40        2      9.86        5.22       0.02
 *
 * Cumulative line energy if the strongest N lines were kept (for reference
 * only -- truncation is NOT how this engine is trimmed; it silences whole
 * bands.  Use AVAS_TYPE_TY_DEC / the cluster span instead):
 *     top   8 lines ->  77.11 % of the line energy
 *     top  16 lines ->  90.35 % of the line energy
 *     top  24 lines ->  94.04 % of the line energy
 *     top  32 lines ->  95.61 % of the line energy
 *     top  48 lines ->  96.93 % of the line energy
 *     top  64 lines ->  97.75 % of the line energy
 *     top  96 lines ->  98.77 % of the line energy
 *     top 128 lines ->  99.37 % of the line energy
 *     top 160 lines ->  99.77 % of the line energy
 *     top 185 lines -> 100.00 % of the line energy
 *
 * Peak of the full 185-line sum: 0.385757 over 60 s of running time (the
 * quasi-periodic beating keeps growing it, so a short window under-reads it),
 * rms 0.095801.  AVAS_TYPE_TY_L1_NORM below normalises the 60 s figure, which is
 * why the running synth cannot clip after the first few seconds.
 * ========================================================================= */

#ifndef _AVAS_SYNTH_TYPE_TY_TABLES_H
#define _AVAS_SYNTH_TYPE_TY_TABLES_H

/* Measured peak of the full sum over 60 s of running time.  This is what
 * AVAS_TYPE_TY_L1_NORM normalises against; see avas_synth_type_ty.h. */
#define AVAS_TYPE_TY_L1_PEAK_ABS            (0.385757f)

#define AVAS_TYPE_TY_L1_TABLE_LINES         (185u)
#define AVAS_TYPE_TY_L1_CLUSTERS            (11u)
#define AVAS_TYPE_TY_L1_MAX_SPAN_HZ         (200.0f)

typedef struct
{
    float freq_hz;
    float amp;        /* linear, as measured; scale by AVAS_TYPE_TY_L1_NORM */
    float phase_rad;  /* -pi..+pi as measured */
} avas_type_ty_l1_line_t;

/* One full-rate carrier per cluster.  `first`/`count` index s_type_ty_l1_line. */
typedef struct
{
    float    carrier_hz;   /* amplitude-weighted centroid of the cluster */
    uint16_t first;        /* first line index in s_type_ty_l1_line */
    uint16_t count;        /* number of lines in this cluster */
} avas_type_ty_l1_cluster_t;

/* The arrays themselves are emitted only where AVAS_TYPE_TY_L1_TABLE_DEFINE_DATA
 * is defined (avas_synth_type_ty.c).  The header is otherwise pulled in by every
 * translation unit that includes avas_synth_type_ty.h for the API, and a
 * `static const` array would then be duplicated per translation unit. */
#if defined(AVAS_TYPE_TY_L1_TABLE_DEFINE_DATA)

static const avas_type_ty_l1_line_t s_type_ty_l1_line[AVAS_TYPE_TY_L1_TABLE_LINES] =
{
    {    112.7208f, 1.48555960e-03f,  2.36875768f },   /*   0  cluster  0 */
    {    118.4171f, 1.56075445e-03f,  1.12248117f },   /*   1  cluster  0 */
    {    119.8280f, 2.25400021e-03f,  1.23961915f },   /*   2  cluster  0 */
    {    122.0211f, 1.41086338e-03f, -1.25621656f },   /*   3  cluster  0 */
    {    125.9570f, 1.91054135e-03f,  3.03677379f },   /*   4  cluster  0 */
    {    126.9478f, 1.92472727e-03f,  1.40248536f },   /*   5  cluster  0 */
    {    140.7005f, 1.75015286e-03f,  2.71185731f },   /*   6  cluster  0 */
    {    142.3025f, 1.73192320e-03f,  0.07382169f },   /*   7  cluster  0 */
    {    145.2006f, 1.50512868e-03f,  1.45310681f },   /*   8  cluster  0 */
    {    147.0225f, 1.68885608e-03f,  1.87563502f },   /*   9  cluster  0 */
    {    150.0722f, 1.81950466e-03f, -2.26118237f },   /*  10  cluster  0 */
    {    161.8467f, 1.96516047e-03f, -1.04592232f },   /*  11  cluster  0 */
    {    167.3785f, 1.36526325e-03f,  0.60856675f },   /*  12  cluster  0 */
    {    197.6154f, 1.46204411e-03f,  3.13822700f },   /*  13  cluster  0 */
    {    210.9696f, 1.41780316e-03f, -0.45443340f },   /*  14  cluster  0 */
    {    211.6659f, 1.46858642e-03f,  0.06924274f },   /*  15  cluster  0 */
    {    219.8838f, 1.46990534e-03f,  0.79789655f },   /*  16  cluster  0 */
    {    223.4331f, 1.59107115e-03f,  0.35866175f },   /*  17  cluster  0 */
    {    232.8431f, 1.57450234e-03f, -0.04530684f },   /*  18  cluster  0 */
    {    234.0090f, 3.07836588e-03f, -2.40056240f },   /*  19  cluster  0 */
    {    236.0230f, 1.79794405e-03f,  1.01139818f },   /*  20  cluster  0 */
    {    241.4714f, 3.40073703e-03f, -0.21222038f },   /*  21  cluster  0 */
    {    242.4609f, 3.05876059e-03f,  2.35822426f },   /*  22  cluster  0 */
    {    243.9095f, 1.54064961e-03f,  0.92251930f },   /*  23  cluster  0 */
    {    300.3572f, 1.23162260e-03f,  1.83962393f },   /*  24  cluster  0 */
    {    322.8006f, 3.77114418e-03f,  2.22099471f },   /*  25  cluster  1 */
    {    324.9378f, 5.36834299e-03f, -2.52777643f },   /*  26  cluster  1 */
    {    329.2358f, 2.07510880e-03f,  2.50063403f },   /*  27  cluster  1 */
    {    331.7971f, 2.10964676e-03f,  1.48266217f },   /*  28  cluster  1 */
    {    334.9633f, 2.81947707e-03f, -1.96756777f },   /*  29  cluster  1 */
    {    338.3062f, 2.17091652e-03f, -0.52622440f },   /*  30  cluster  1 */
    {    356.4833f, 1.25752424e-03f,  0.46475113f },   /*  31  cluster  1 */
    {    360.4442f, 1.96634363e-03f,  2.86998137f },   /*  32  cluster  1 */
    {    361.3743f, 2.59987423e-03f, -0.93046306f },   /*  33  cluster  1 */
    {    363.4740f, 2.22980877e-03f, -0.41923014f },   /*  34  cluster  1 */
    {    365.0449f, 2.02126792e-03f,  1.16528277f },   /*  35  cluster  1 */
    {    412.0799f, 1.67871354e-03f,  3.11275410f },   /*  36  cluster  1 */
    {    413.7371f, 2.07872468e-03f,  1.27470050f },   /*  37  cluster  1 */
    {    417.2556f, 3.80032024e-03f, -2.82949277f },   /*  38  cluster  1 */
    {    420.9893f, 3.36101154e-03f,  1.35829298f },   /*  39  cluster  1 */
    {    437.0875f, 1.93196431e-03f,  0.13492872f },   /*  40  cluster  1 */
    {    442.5626f, 2.39992707e-03f, -2.62868899f },   /*  41  cluster  1 */
    {    464.9050f, 3.93773218e-03f, -1.15222540f },   /*  42  cluster  1 */
    {    475.9683f, 6.21412772e-03f,  1.65353921f },   /*  43  cluster  1 */
    {    477.5586f, 1.02678843e-02f,  0.42760853f },   /*  44  cluster  1 */
    {    479.6669f, 1.49275710e-02f,  0.47343855f },   /*  45  cluster  1 */
    {    482.9487f, 3.79271096e-02f, -0.18631844f },   /*  46  cluster  1 */
    {    484.4144f, 2.53252281e-02f,  1.76629423f },   /*  47  cluster  1 */
    {    487.6635f, 1.57354494e-02f,  0.61150563f },   /*  48  cluster  1 */
    {    490.3477f, 1.38382354e-02f, -0.56597620f },   /*  49  cluster  1 */
    {    513.2380f, 2.87147645e-03f,  1.07928730f },   /*  50  cluster  1 */
    {    514.4256f, 2.85640754e-03f,  2.00744697f },   /*  51  cluster  1 */
    {    524.3850f, 2.26142410e-03f,  0.44556821f },   /*  52  cluster  2 */
    {    525.6797f, 2.24213626e-03f,  0.82342322f },   /*  53  cluster  2 */
    {    527.7368f, 2.24812981e-03f,  0.40288636f },   /*  54  cluster  2 */
    {    530.1166f, 3.79332033e-03f, -1.00483950f },   /*  55  cluster  2 */
    {    531.1739f, 1.92050756e-03f,  1.64409312f },   /*  56  cluster  2 */
    {    535.2366f, 3.27744055e-03f,  1.96237239f },   /*  57  cluster  2 */
    {    537.2342f, 1.93830618e-03f, -1.34139877f },   /*  58  cluster  2 */
    {    551.8926f, 1.26645731e-03f,  0.69371396f },   /*  59  cluster  2 */
    {    586.4036f, 1.66621167e-03f, -1.18802477f },   /*  60  cluster  2 */
    {    596.1019f, 1.56987012e-03f, -2.25949952f },   /*  61  cluster  2 */
    {    598.2463f, 1.57768940e-03f, -1.92196357f },   /*  62  cluster  2 */
    {    613.6889f, 3.98812981e-03f,  2.69222410f },   /*  63  cluster  2 */
    {    620.2397f, 7.89641098e-03f,  1.79536032f },   /*  64  cluster  2 */
    {    622.4595f, 5.69471833e-03f,  1.79052413f },   /*  65  cluster  2 */
    {    628.6780f, 8.29794019e-03f, -1.37715128f },   /*  66  cluster  2 */
    {    644.4101f, 9.49196529e-03f, -2.15098674f },   /*  67  cluster  2 */
    {    647.6064f, 1.36085360e-02f, -0.15549055f },   /*  68  cluster  2 */
    {    648.7955f, 1.01492018e-02f,  2.86548073f },   /*  69  cluster  2 */
    {    650.0922f, 2.37476395e-02f,  2.85350280f },   /*  70  cluster  2 */
    {    652.0285f, 1.94484443e-02f, -0.41600943f },   /*  71  cluster  2 */
    {    657.0172f, 1.84503952e-02f,  2.82277733f },   /*  72  cluster  2 */
    {    692.6298f, 2.01165327e-02f, -0.93824471f },   /*  73  cluster  2 */
    {    713.4602f, 2.76485140e-02f, -0.66541162f },   /*  74  cluster  2 */
    {    715.4079f, 2.78385353e-02f,  1.61065346f },   /*  75  cluster  2 */
    {    718.4839f, 2.89177607e-02f,  2.98187779f },   /*  76  cluster  2 */
    {    721.7450f, 4.01565184e-02f,  1.49109774f },   /*  77  cluster  2 */
    {    723.4217f, 8.66246135e-02f,  3.03429004f },   /*  78  cluster  2 */
    {    725.2120f, 3.44917161e-02f, -1.36574251f },   /*  79  cluster  3 */
    {    754.5803f, 9.57369872e-03f, -3.12762943f },   /*  80  cluster  3 */
    {    760.0440f, 6.72248433e-03f, -1.81086048f },   /*  81  cluster  3 */
    {    797.1870f, 2.91376964e-03f,  0.27555074f },   /*  82  cluster  3 */
    {    802.8056f, 3.02935142e-03f, -1.14526408f },   /*  83  cluster  3 */
    {    805.1406f, 1.57475695e-03f, -1.14078126f },   /*  84  cluster  3 */
    {    827.3881f, 4.37922372e-03f,  0.37465448f },   /*  85  cluster  3 */
    {    834.4949f, 6.02969013e-03f, -0.67033671f },   /*  86  cluster  3 */
    {    841.0494f, 3.33647080e-03f,  1.94042468f },   /*  87  cluster  3 */
    {    842.1492f, 3.05249474e-03f, -0.89219959f },   /*  88  cluster  3 */
    {    843.2279f, 4.31843845e-03f, -3.10836596f },   /*  89  cluster  3 */
    {    844.3270f, 2.91648059e-03f, -0.77673202f },   /*  90  cluster  3 */
    {    853.4626f, 3.16733489e-03f, -1.60184383f },   /*  91  cluster  3 */
    {    854.6502f, 3.02593944e-03f,  1.69995423f },   /*  92  cluster  3 */
    {    866.7796f, 2.78640919e-03f, -1.64174824f },   /*  93  cluster  3 */
    {    875.3551f, 1.69762555e-03f,  0.85830555f },   /*  94  cluster  3 */
    {    878.0558f, 1.52386043e-03f, -2.45070757f },   /*  95  cluster  3 */
    {    881.9476f, 1.53824337e-03f,  0.43546423f },   /*  96  cluster  3 */
    {    904.9032f, 1.21530351e-03f,  0.52621193f },   /*  97  cluster  3 */
    {    966.0761f, 1.87574747e-03f, -1.29062230f },   /*  98  cluster  4 */
    {    967.7684f, 2.45547056e-03f, -2.06767304f },   /*  99  cluster  4 */
    {    969.9159f, 2.54522712e-03f,  0.39798934f },   /* 100  cluster  4 */
    {   1021.9175f, 1.21147725e-03f, -1.70248875f },   /* 101  cluster  4 */
    {   1084.7779f, 1.46310486e-03f,  0.11834461f },   /* 102  cluster  4 */
    {   1087.2825f, 1.56996322e-03f, -1.83569156f },   /* 103  cluster  4 */
    {   1126.3322f, 1.33066437e-03f, -1.94152776f },   /* 104  cluster  4 */
    {   1128.1818f, 1.56621567e-03f,  0.43234621f },   /* 105  cluster  4 */
    {   1129.6232f, 1.35714266e-03f,  1.21820184f },   /* 106  cluster  4 */
    {   1133.1203f, 1.21861326e-03f,  0.97885697f },   /* 107  cluster  4 */
    {   1146.1663f, 1.84056427e-03f, -0.29336391f },   /* 108  cluster  4 */
    {   1150.9242f, 1.50028876e-03f, -2.41379279f },   /* 109  cluster  4 */
    {   1154.2647f, 1.41980081e-03f,  0.56471984f },   /* 110  cluster  4 */
    {   1169.5911f, 1.63391616e-03f, -0.64059190f },   /* 111  cluster  5 */
    {   1195.5853f, 1.21912271e-03f, -0.19003963f },   /* 112  cluster  5 */
    {   1199.1937f, 1.48344401e-03f, -2.33063963f },   /* 113  cluster  5 */
    {   1201.1599f, 1.55353776e-03f,  0.14053606f },   /* 114  cluster  5 */
    {   1203.2389f, 1.33805966e-03f,  2.20568029f },   /* 115  cluster  5 */
    {   1214.8228f, 1.62625502e-03f,  1.27961687f },   /* 116  cluster  5 */
    {   1222.0483f, 1.62249221e-03f,  1.06245731f },   /* 117  cluster  5 */
    {   1230.4567f, 2.79332957e-03f,  0.20944547f },   /* 118  cluster  5 */
    {   1237.1538f, 2.16085860e-03f,  2.43519741f },   /* 119  cluster  5 */
    {   1243.3832f, 1.93770005e-03f, -1.85807876f },   /* 120  cluster  5 */
    {   1245.5371f, 2.44653836e-03f, -0.63668797f },   /* 121  cluster  5 */
    {   1251.2072f, 1.94581452e-03f, -0.12351323f },   /* 122  cluster  5 */
    {   1267.7286f, 2.14028439e-03f, -1.48327114f },   /* 123  cluster  5 */
    {   1289.9032f, 1.56078894e-03f, -1.13611816f },   /* 124  cluster  5 */
    {   1295.3840f, 1.45825445e-03f, -2.75397726f },   /* 125  cluster  5 */
    {   1319.6537f, 1.65683637e-03f, -2.36223424f },   /* 126  cluster  5 */
    {   1329.2695f, 1.31443253e-03f,  0.49564603f },   /* 127  cluster  5 */
    {   1330.4301f, 1.26760653e-03f,  3.08082501f },   /* 128  cluster  5 */
    {   1333.2968f, 1.36977119e-03f, -0.35388493f },   /* 129  cluster  5 */
    {   1374.8276f, 1.35745351e-03f,  2.27987551f },   /* 130  cluster  6 */
    {   1380.2974f, 1.45752906e-03f,  0.87382835f },   /* 131  cluster  6 */
    {   1389.3418f, 1.76156748e-03f, -2.35837591f },   /* 132  cluster  6 */
    {   1392.3603f, 2.64175290e-03f, -1.90796428f },   /* 133  cluster  6 */
    {   1394.0885f, 2.51146839e-03f,  1.41429678f },   /* 134  cluster  6 */
    {   1397.8584f, 2.77586722e-03f,  2.24141549f },   /* 135  cluster  6 */
    {   1413.9818f, 5.28846080e-03f, -0.76634762f },   /* 136  cluster  6 */
    {   1417.1023f, 3.53986310e-03f,  2.17579680f },   /* 137  cluster  6 */
    {   1445.7596f, 7.36674115e-03f,  2.54507968f },   /* 138  cluster  6 */
    {   1447.1906f, 9.98408776e-03f, -2.22659812f },   /* 139  cluster  6 */
    {   1448.2386f, 9.19806689e-03f,  2.28940400f },   /* 140  cluster  6 */
    {   1453.5371f, 6.05668949e-03f,  2.83070607f },   /* 141  cluster  6 */
    {   1465.6943f, 3.21243396e-03f,  0.31011192f },   /* 142  cluster  6 */
    {   1467.7165f, 3.35156806e-03f,  1.92259488f },   /* 143  cluster  6 */
    {   1477.7781f, 2.27033844e-03f, -1.65357215f },   /* 144  cluster  6 */
    {   1482.1429f, 2.17772477e-03f, -3.06061482f },   /* 145  cluster  6 */
    {   1489.1469f, 2.11286873e-03f, -3.10124001f },   /* 146  cluster  6 */
    {   1510.9230f, 1.98867141e-03f, -0.60461551f },   /* 147  cluster  6 */
    {   1512.4299f, 2.07708102e-03f,  0.50956379f },   /* 148  cluster  6 */
    {   1515.1007f, 2.52254254e-03f, -0.34693559f },   /* 149  cluster  6 */
    {   1524.5434f, 2.92672695e-03f,  0.83714687f },   /* 150  cluster  6 */
    {   1536.9195f, 3.80680026e-03f,  1.80527269f },   /* 151  cluster  6 */
    {   1541.0560f, 5.03360635e-03f,  2.36938007f },   /* 152  cluster  6 */
    {   1554.5108f, 4.05630354e-03f, -0.20498643f },   /* 153  cluster  6 */
    {   1560.5472f, 4.08805044e-03f, -1.77480956f },   /* 154  cluster  6 */
    {   1570.5202f, 4.21189812e-03f, -3.11747872f },   /* 155  cluster  6 */
    {   1572.2132f, 3.54954258e-03f,  0.54530102f },   /* 156  cluster  6 */
    {   1574.5226f, 3.53745078e-03f,  0.10601573f },   /* 157  cluster  6 */
    {   1595.5621f, 2.39503836e-03f, -2.64882716f },   /* 158  cluster  7 */
    {   1604.6000f, 3.30287738e-03f, -1.14285440f },   /* 159  cluster  7 */
    {   1625.8238f, 2.17680139e-03f, -1.94694299f },   /* 160  cluster  7 */
    {   1627.6940f, 2.45188219e-03f, -0.46125734f },   /* 161  cluster  7 */
    {   1653.5974f, 2.36883408e-03f,  0.82898758f },   /* 162  cluster  7 */
    {   1666.9009f, 2.03295109e-03f, -3.13289495f },   /* 163  cluster  7 */
    {   1689.1400f, 1.51906625e-03f,  2.42603496f },   /* 164  cluster  7 */
    {   1706.1713f, 2.00276746e-03f, -2.72117009f },   /* 165  cluster  7 */
    {   1714.9673f, 1.69083028e-03f, -2.85341194f },   /* 166  cluster  7 */
    {   1723.9398f, 1.44604660e-03f,  1.46922750f },   /* 167  cluster  7 */
    {   1937.0804f, 2.89342518e-03f, -1.08075789f },   /* 168  cluster  8 */
    {   1943.2689f, 2.22733892e-03f,  2.03814529f },   /* 169  cluster  8 */
    {   1949.4514f, 2.03524214e-03f, -1.24045373f },   /* 170  cluster  8 */
    {   1951.0837f, 1.56338711e-03f,  2.17783361f },   /* 171  cluster  8 */
    {   1956.2599f, 1.60171919e-03f,  2.57661013f },   /* 172  cluster  8 */
    {   1958.4415f, 1.89705482e-03f, -2.11094238f },   /* 173  cluster  8 */
    {   1966.5431f, 1.32775586e-03f,  1.79252730f },   /* 174  cluster  8 */
    {   2111.2053f, 1.21369325e-03f,  2.02678339f },   /* 175  cluster  8 */
    {   2122.4976f, 1.52111860e-03f,  0.60369306f },   /* 176  cluster  8 */
    {   2138.7643f, 1.76230773e-03f, -2.92952547f },   /* 177  cluster  9 */
    {   2166.4224f, 1.34153260e-03f,  1.94413477f },   /* 178  cluster  9 */
    {   2169.2167f, 3.25643535e-03f, -0.76443108f },   /* 179  cluster  9 */
    {   2173.8329f, 2.61381648e-03f, -0.56943422f },   /* 180  cluster  9 */
    {   2176.1609f, 1.82967092e-03f,  0.35127209f },   /* 181  cluster  9 */
    {   2179.3485f, 1.21623011e-03f,  0.90009788f },   /* 182  cluster  9 */
    {   2904.7476f, 1.50196645e-03f,  2.65718281f },   /* 183  cluster 10 */
    {   2914.6123f, 1.33844054e-03f,  1.10679441f },   /* 184  cluster 10 */
};

static const avas_type_ty_l1_cluster_t s_type_ty_l1_cluster[AVAS_TYPE_TY_L1_CLUSTERS] =
{
    {    185.9943f,   0u,  25u },   /*  0    112.7 ..   300.4 Hz */
    {    456.0453f,  25u,  27u },   /*  1    322.8 ..   514.4 Hz */
    {    683.2655f,  52u,  27u },   /*  2    524.4 ..   723.4 Hz */
    {    786.1871f,  79u,  19u },   /*  3    725.2 ..   904.9 Hz */
    {   1069.7642f,  98u,  13u },   /*  4    966.1 ..  1154.3 Hz */
    {   1249.5151f, 111u,  19u },   /*  5   1169.6 ..  1333.3 Hz */
    {   1477.6251f, 130u,  28u },   /*  6   1374.8 ..  1574.5 Hz */
    {   1652.0551f, 158u,  10u },   /*  7   1595.6 ..  1723.9 Hz */
    {   1977.9012f, 168u,   9u },   /*  8   1937.1 ..  2122.5 Hz */
    {   2167.5261f, 177u,   6u },   /*  9   2138.8 ..  2179.3 Hz */
    {   2909.3960f, 183u,   2u },   /* 10   2904.7 ..  2914.6 Hz */
};

#endif  //defined(AVAS_TYPE_TY_L1_TABLE_DEFINE_DATA)

#endif  //!_AVAS_SYNTH_TYPE_TY_TABLES_H
