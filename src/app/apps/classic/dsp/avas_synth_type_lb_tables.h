/* =========================================================================
 * Type_LB AVAS : L3 line-model coefficient table  (GENERATED -- DO NOT EDIT)
 *
 * GENERATED, and the generator is not part of this repository: this table is the
 * shipped artifact, not something to hand-edit or re-derive here.
 *
 * Model, the TONE half of L3:
 *     tone(t) = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])
 *
 * 264 lines, 34.75 .. 1121.83 Hz.  The whole set is here -- L3 is not truncated
 * (the L4/L5/L8 variants of the same coefficient set take the top 64).
 *
 * WHY THIS HAS THE SAME SHAPE AS THE TYPE_TY L1 TABLE.  The two voices are the same
 * equation with different coefficients, which is why avas_synth_type_lb.c and
 * avas_synth_type_ty.c compute the tone half identically.  The Type_LB's energy
 * all sits below 1.2 kHz, so it needs FEWER carriers than the Type_TY despite having
 * more lines.
 *
 * PHASES ARE WRAPPED HERE, and the parameter file's are not.  MEASURED: the input
 * spans 6.04 to 49.04 rad, up to eight turns, where the Type_TY L1 parameter set
 * happens to carry wrapped phases.  cos is 2*pi-periodic so the reduction is exact
 * -- checked by the fact that the peak and rms below reproduce the parameter file's
 * own figures from the RAW values -- but the firmware cannot take them raw:
 * audio_fast_sinf_0_to_2pi() is a parabolic approximation valid on one turn, so an
 * entry eight turns out is not a small error, it is a wrong oscillator.
 *
 * ORDER: frequency ascending.  Each cluster below is a contiguous run of entries,
 * so the firmware needs no per-line cluster index.
 *
 * CLUSTER DECOMPOSITION  (max span 100 Hz -> 7 clusters)
 * Per cluster, exactly:
 *     sum_j A_j cos(2 pi f_j t + p_j) = Re{ e^{i 2 pi fc t} * Z(t) },
 *     Z(t) = sum_j A_j e^{i (2 pi (f_j - fc) t + p_j)}
 * Z is band-limited to the cluster half-span, so the firmware rebuilds it once
 * every AVAS_TYPE_LB_DEC samples and linearly interpolates between rebuilds, while
 * only the 7 carriers run at 48 kHz.  All 264 lines stay alive.
 * fc is the amplitude-weighted centroid (smallest baseband offset on the strongest
 * lines = smallest interpolation error).
 *
 * MAX SPAN is 100 Hz and not the Type_TY table's 200.  Two of the four reasons the
 * dsPIC33CK (Q15) port of this voice chose 100 were fixed-point reasons that do not
 * apply to a float engine, but the envelope interpolation error is not one of them
 * (max |f - fc| would be 136.5 Hz at span 200 against 60.7 Hz here), and keeping
 * the span identical is what keeps the two boards' voices the same sound.
 *
 *   #   carrier Hz    lines   span Hz   max |f-fc|   energy %
 *   0       66.82       50     79.74       47.67      85.31
 *   1      197.34       73     89.07       60.66       9.61
 *   2      319.86       64     96.73       51.64       4.35
 *   3      454.73       24     61.73       43.55       0.19
 *   4      590.23        2      1.53        0.82       0.01
 *   5      987.73       50     78.92       52.88       0.53
 *   6     1121.83        1      0.00        0.00       0.00
 *
 * Two of these clusters are runts -- see the sizes above.  A rule that folded them
 * into a neighbour would return their carriers, but it would be a NEW clustering
 * rule for both voices and the load fits without it.  Priced, not taken.
 *
 * Cumulative line energy if the strongest N lines were kept (for reference only --
 * truncation is NOT how this engine is trimmed; it silences whole bands):
 *     top   8 lines ->  86.31 % of the line energy
 *     top  16 lines ->  92.30 % of the line energy
 *     top  24 lines ->  95.01 % of the line energy
 *     top  32 lines ->  96.38 % of the line energy
 *     top  48 lines ->  97.47 % of the line energy
 *     top  64 lines ->  98.19 % of the line energy
 *     top  96 lines ->  98.88 % of the line energy
 *     top 128 lines ->  99.27 % of the line energy
 *     top 192 lines ->  99.75 % of the line energy
 *     top 264 lines -> 100.00 % of the line energy
 *
 * Peak of the full 264-line sum: 0.631084 over 60 s of running time (the
 * quasi-periodic beating keeps growing it, so a short window under-reads it),
 * rms 0.210606.
 *
 * THE NORMALISATION IS ON THE SUM, NOT ON THE TONE.  AVAS_TYPE_LB_L3_PEAK_ABS below
 * is the tone's 60 s peak PLUS the noise bank's measured peak, because the two
 * halves are added before the gain.  The Q15 port normalised the tone alone and had
 * to add a saturating output arm once tone+noise was measured to clip.
 * ========================================================================= */

#ifndef _AVAS_SYNTH_TYPE_LB_TABLES_H
#define _AVAS_SYNTH_TYPE_LB_TABLES_H

/* Measured peak of the tone alone over 60 s of running time. */
#define AVAS_TYPE_LB_L3_TONE_PEAK_ABS    (0.631084f)
/* Measured peak of the noise bank alone, gusts included. */
#define AVAS_TYPE_LB_L3_NOISE_PEAK_ABS   (0.119694f)
/* What the output is normalised against: the bound on tone + noise. */
#define AVAS_TYPE_LB_L3_PEAK_ABS         (0.750779f)

#define AVAS_TYPE_LB_L3_TABLE_LINES      (264u)
#define AVAS_TYPE_LB_L3_CLUSTERS         (7u)
#define AVAS_TYPE_LB_L3_MAX_SPAN_HZ      (100.0f)

typedef struct
{
    float freq_hz;
    float amp;        /* linear, as measured; scale by AVAS_TYPE_LB_L3_NORM */
    float phase_rad;  /* -pi..+pi; see the phase note above */
} avas_type_lb_l3_line_t;

/* One full-rate carrier per cluster.  `first`/`count` index s_type_lb_l3_line. */
typedef struct
{
    float    carrier_hz;   /* amplitude-weighted centroid of the cluster */
    uint16_t first;        /* first line index in s_type_lb_l3_line */
    uint16_t count;        /* number of lines in this cluster */
} avas_type_lb_l3_cluster_t;

/* The arrays are emitted only where AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA is defined
 * (avas_synth_type_lb.c).  The header is otherwise pulled in by every translation
 * unit that includes avas_synth_type_lb.h for the API, and a `static const` array
 * would then be duplicated per translation unit. */
#if defined(AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA)

static const avas_type_lb_l3_line_t s_type_lb_l3_line[AVAS_TYPE_LB_L3_TABLE_LINES] =
{
    {     34.7508f, 1.62008591e-03f,  1.81693017f },   /*   0  cluster  0 */
    {     49.8101f, 2.99222971e-03f, -1.89868982f },   /*   1  cluster  0 */
    {     50.7614f, 2.55439683e-03f,  2.80391333f },   /*   2  cluster  0 */
    {     51.2836f, 3.41346356e-03f, -0.92899179f },   /*   3  cluster  0 */
    {     51.9686f, 4.75431154e-03f,  2.01718604f },   /*   4  cluster  0 */
    {     52.6851f, 6.50058232e-03f, -1.64917936f },   /*   5  cluster  0 */
    {     53.4762f, 7.30967543e-03f, -0.62272659f },   /*   6  cluster  0 */
    {     53.9975f, 1.56186331e-02f, -0.46572213f },   /*   7  cluster  0 */
    {     54.2841f, 1.87149123e-02f,  1.07761094f },   /*   8  cluster  0 */
    {     54.5782f, 4.12005786e-02f,  1.55268260f },   /*   9  cluster  0 */
    {     54.9243f, 2.59565417e-01f, -1.22173121f },   /*  10  cluster  0 */
    {     55.2360f, 4.05667450e-02f,  1.45821655f },   /*  11  cluster  0 */
    {     55.4955f, 3.24430754e-02f, -0.52294338f },   /*  12  cluster  0 */
    {     55.8182f, 2.30018579e-02f,  2.65990899f },   /*  13  cluster  0 */
    {     56.4308f, 1.11800226e-02f,  2.61508804f },   /*  14  cluster  0 */
    {     56.7621f, 9.31447001e-03f,  0.93717039f },   /*  15  cluster  0 */
    {     57.3331f, 5.95925293e-03f, -3.04490286f },   /*  16  cluster  0 */
    {     57.9745f, 2.58689865e-03f,  2.19724460f },   /*  17  cluster  0 */
    {     58.6581f, 2.80550656e-03f,  2.75740815f },   /*  18  cluster  0 */
    {     59.2742f, 2.46703017e-03f,  1.56698923f },   /*  19  cluster  0 */
    {     59.9044f, 2.35514518e-03f,  0.94540171f },   /*  20  cluster  0 */
    {     60.7999f, 2.36900911e-03f,  0.74721563f },   /*  21  cluster  0 */
    {     74.4053f, 1.91863263e-03f,  0.32117553f },   /*  22  cluster  0 */
    {     76.6856f, 1.88467049e-03f,  1.34720512f },   /*  23  cluster  0 */
    {     77.2361f, 2.46069560e-03f, -2.36204140f },   /*  24  cluster  0 */
    {     79.5051f, 1.57745359e-03f,  1.29199832f },   /*  25  cluster  0 */
    {     80.7326f, 2.03767696e-03f, -2.34058985f },   /*  26  cluster  0 */
    {     81.4086f, 2.57293947e-03f, -0.04084059f },   /*  27  cluster  0 */
    {     82.2322f, 5.64197333e-03f,  0.49600329f },   /*  28  cluster  0 */
    {     82.5448f, 7.78413713e-03f, -2.99159304f },   /*  29  cluster  0 */
    {     82.8632f, 2.29155136e-03f, -0.90146699f },   /*  30  cluster  0 */
    {     83.2040f, 1.54064281e-03f,  2.06024643f },   /*  31  cluster  0 */
    {    107.2037f, 1.75915466e-03f, -1.92331295f },   /*  32  cluster  0 */
    {    107.4808f, 1.76446478e-03f, -1.87639386f },   /*  33  cluster  0 */
    {    108.4692f, 3.93062407e-03f, -1.15642392f },   /*  34  cluster  0 */
    {    108.9871f, 3.24964667e-03f, -2.88813755f },   /*  35  cluster  0 */
    {    109.3060f, 9.92555579e-03f,  0.47785910f },   /*  36  cluster  0 */
    {    109.5935f, 6.78569757e-03f, -2.83548834f },   /*  37  cluster  0 */
    {    109.9072f, 2.97269542e-02f, -2.58404632f },   /*  38  cluster  0 */
    {    110.5601f, 7.25713296e-03f, -2.22199570f },   /*  39  cluster  0 */
    {    111.0934f, 6.10815175e-03f,  2.63071094f },   /*  40  cluster  0 */
    {    111.4311f, 3.22183028e-03f, -2.92543223f },   /*  41  cluster  0 */
    {    111.7578f, 1.29481818e-02f,  2.81664336f },   /*  42  cluster  0 */
    {    112.3501f, 2.40638189e-02f,  1.82012724f },   /*  43  cluster  0 */
    {    112.8898f, 4.06176682e-03f, -1.20460213f },   /*  44  cluster  0 */
    {    113.2428f, 2.89596966e-03f,  2.05527179f },   /*  45  cluster  0 */
    {    113.5965f, 2.20069705e-03f, -0.30766794f },   /*  46  cluster  0 */
    {    113.8625f, 2.16072492e-03f, -0.49572314f },   /*  47  cluster  0 */
    {    114.1761f, 1.54921308e-03f,  2.87668849f },   /*  48  cluster  0 */
    {    114.4913f, 1.54158703e-03f,  2.83382635f },   /*  49  cluster  0 */
    {    136.6869f, 2.78606634e-03f, -0.90918680f },   /*  50  cluster  1 */
    {    137.2622f, 2.77108308e-03f, -0.87930426f },   /*  51  cluster  1 */
    {    137.6519f, 5.87450511e-03f, -0.69946450f },   /*  52  cluster  1 */
    {    138.5458f, 1.87781997e-03f,  1.85275215f },   /*  53  cluster  1 */
    {    139.7063f, 1.81053670e-03f,  1.32609840f },   /*  54  cluster  1 */
    {    140.0797f, 2.74899468e-03f, -1.32975556f },   /*  55  cluster  1 */
    {    140.6023f, 2.50930066e-03f, -2.12217951f },   /*  56  cluster  1 */
    {    140.9295f, 1.63838422e-03f,  0.32326864f },   /*  57  cluster  1 */
    {    141.5603f, 2.04822484e-03f,  2.13719077f },   /*  58  cluster  1 */
    {    142.7746f, 2.38910824e-03f,  2.95494847f },   /*  59  cluster  1 */
    {    143.6936f, 2.51454704e-03f,  2.85814799f },   /*  60  cluster  1 */
    {    144.2617f, 4.23659118e-03f,  0.83791208f },   /*  61  cluster  1 */
    {    144.9245f, 3.26818479e-03f, -1.76635195f },   /*  62  cluster  1 */
    {    145.2688f, 2.92027898e-03f, -2.86074587f },   /*  63  cluster  1 */
    {    146.1083f, 6.73771685e-03f, -2.22024165f },   /*  64  cluster  1 */
    {    146.9183f, 4.00770431e-03f, -0.76940368f },   /*  65  cluster  1 */
    {    147.1900f, 4.36710233e-03f, -0.16639507f },   /*  66  cluster  1 */
    {    148.0584f, 2.92480425e-03f,  2.34970133f },   /*  67  cluster  1 */
    {    150.4286f, 2.67515229e-03f,  0.26415413f },   /*  68  cluster  1 */
    {    151.1145f, 3.09340757e-03f,  1.39894957f },   /*  69  cluster  1 */
    {    151.3752f, 4.56036104e-03f, -1.51299408f },   /*  70  cluster  1 */
    {    162.4560f, 2.38817426e-03f,  1.06789534f },   /*  71  cluster  1 */
    {    162.9448f, 2.71215108e-03f,  2.32849540f },   /*  72  cluster  1 */
    {    163.5360f, 2.95944854e-03f, -3.08309560f },   /*  73  cluster  1 */
    {    163.8635f, 3.08595461e-03f, -2.55072039f },   /*  74  cluster  1 */
    {    164.3820f, 9.02561113e-03f,  0.60927739f },   /*  75  cluster  1 */
    {    164.8459f, 2.68024625e-02f, -2.35432365f },   /*  76  cluster  1 */
    {    165.1892f, 1.99232018e-02f,  1.51863452f },   /*  77  cluster  1 */
    {    166.3111f, 5.18748661e-03f,  2.83286372f },   /*  78  cluster  1 */
    {    166.6715f, 5.03893193e-03f,  2.66508502f },   /*  79  cluster  1 */
    {    167.2043f, 3.24244383e-03f, -0.96422986f },   /*  80  cluster  1 */
    {    168.1467f, 1.70837896e-02f,  1.23651492f },   /*  81  cluster  1 */
    {    168.5213f, 1.36586464e-02f, -0.59443756f },   /*  82  cluster  1 */
    {    169.2186f, 2.96768600e-03f, -1.01816577f },   /*  83  cluster  1 */
    {    169.5486f, 3.79614352e-03f,  1.15490776f },   /*  84  cluster  1 */
    {    170.0250f, 2.41145118e-03f, -2.64575091f },   /*  85  cluster  1 */
    {    173.8191f, 1.54731120e-03f,  0.18148236f },   /*  86  cluster  1 */
    {    192.6615f, 1.54095255e-03f,  2.46060309f },   /*  87  cluster  1 */
    {    209.4999f, 1.75777221e-03f, -0.08411364f },   /*  88  cluster  1 */
    {    213.0756f, 3.32784679e-03f, -0.26512412f },   /*  89  cluster  1 */
    {    213.4101f, 2.87567217e-03f, -1.46612925f },   /*  90  cluster  1 */
    {    213.6773f, 3.28480989e-03f, -0.38025745f },   /*  91  cluster  1 */
    {    214.0122f, 2.73030998e-03f, -1.91667764f },   /*  92  cluster  1 */
    {    214.5623f, 3.93108502e-03f,  1.31209645f },   /*  93  cluster  1 */
    {    214.8848f, 7.30621208e-03f,  0.44646874f },   /*  94  cluster  1 */
    {    215.2013f, 7.72290631e-03f,  0.96245596f },   /*  95  cluster  1 */
    {    215.4951f, 6.55768156e-03f, -0.34396835f },   /*  96  cluster  1 */
    {    215.8501f, 3.87884169e-03f,  1.46445840f },   /*  97  cluster  1 */
    {    216.4175f, 3.66058843e-03f,  0.99309582f },   /*  98  cluster  1 */
    {    216.7257f, 9.13031677e-03f,  2.64860936f },   /*  99  cluster  1 */
    {    217.0054f, 6.65436792e-03f, -0.09897135f },   /* 100  cluster  1 */
    {    217.3395f, 1.56799772e-02f,  1.06261567f },   /* 101  cluster  1 */
    {    217.7247f, 6.70478249e-03f, -0.03341368f },   /* 102  cluster  1 */
    {    218.5474f, 1.40177652e-02f,  0.08928280f },   /* 103  cluster  1 */
    {    218.8233f, 1.97033909e-02f, -3.06579490f },   /* 104  cluster  1 */
    {    219.1772f, 3.78172200e-02f,  2.68594442f },   /* 105  cluster  1 */
    {    219.4310f, 1.84332682e-02f,  2.72738458f },   /* 106  cluster  1 */
    {    219.7439f, 2.86237203e-02f,  0.26667402f },   /* 107  cluster  1 */
    {    220.0700f, 2.66594869e-02f, -0.35081184f },   /* 108  cluster  1 */
    {    220.3611f, 2.62817181e-02f, -0.47477253f },   /* 109  cluster  1 */
    {    220.6850f, 2.99157778e-02f,  0.78338909f },   /* 110  cluster  1 */
    {    221.2706f, 8.85527332e-03f,  2.81351885f },   /* 111  cluster  1 */
    {    221.6109f, 4.58992684e-03f, -2.93859325f },   /* 112  cluster  1 */
    {    221.9204f, 9.83157360e-03f, -2.64169593f },   /* 113  cluster  1 */
    {    222.4595f, 6.85265721e-03f,  0.71724076f },   /* 114  cluster  1 */
    {    223.1674f, 2.84252101e-03f, -0.41697511f },   /* 115  cluster  1 */
    {    223.5814f, 2.27688717e-03f, -2.34421858f },   /* 116  cluster  1 */
    {    223.9179f, 4.64325339e-03f, -2.25797282f },   /* 117  cluster  1 */
    {    224.4544f, 2.52472818e-03f, -1.02905723f },   /* 118  cluster  1 */
    {    224.8097f, 2.89992179e-03f,  0.28168564f },   /* 119  cluster  1 */
    {    225.1964f, 3.44331168e-03f,  1.96948430f },   /* 120  cluster  1 */
    {    225.4842f, 1.62093183e-03f,  1.75267378f },   /* 121  cluster  1 */
    {    225.7526f, 1.55523765e-03f,  1.42162463f },   /* 122  cluster  1 */
    {    274.7685f, 2.95100572e-03f,  1.06934206f },   /* 123  cluster  2 */
    {    275.1045f, 1.78786714e-03f,  2.95787204f },   /* 124  cluster  2 */
    {    275.4110f, 2.37906043e-03f, -1.02786591f },   /* 125  cluster  2 */
    {    286.2172f, 1.74563102e-03f,  1.27698518f },   /* 126  cluster  2 */
    {    288.0629f, 1.72695840e-03f,  1.35675190f },   /* 127  cluster  2 */
    {    288.9336f, 2.07191538e-03f,  2.93855463f },   /* 128  cluster  2 */
    {    289.2252f, 2.27876111e-03f, -2.51809498f },   /* 129  cluster  2 */
    {    289.5883f, 2.69124357e-03f, -1.05241930f },   /* 130  cluster  2 */
    {    290.1499f, 1.65776421e-03f,  1.05761765f },   /* 131  cluster  2 */
    {    290.4917f, 1.61678412e-03f,  0.79176570f },   /* 132  cluster  2 */
    {    290.8652f, 2.73541008e-03f, -0.87557399f },   /* 133  cluster  2 */
    {    292.0110f, 3.26968708e-03f,  0.68428867f },   /* 134  cluster  2 */
    {    292.3287f, 6.49199264e-03f,  0.34835221f },   /* 135  cluster  2 */
    {    292.6590f, 3.37827543e-03f,  0.98618727f },   /* 136  cluster  2 */
    {    293.2328f, 6.32605561e-03f,  0.21653412f },   /* 137  cluster  2 */
    {    293.5363f, 6.88022296e-03f,  0.96213195f },   /* 138  cluster  2 */
    {    294.1587f, 7.87629756e-03f,  2.09775646f },   /* 139  cluster  2 */
    {    294.4614f, 7.36617848e-03f,  1.53871877f },   /* 140  cluster  2 */
    {    295.2856f, 5.17420359e-03f, -1.46680240f },   /* 141  cluster  2 */
    {    295.6547f, 6.28267016e-03f,  0.17296690f },   /* 142  cluster  2 */
    {    295.9711f, 3.32128892e-03f,  0.82230557f },   /* 143  cluster  2 */
    {    296.2849f, 3.78104763e-03f,  1.94841541f },   /* 144  cluster  2 */
    {    296.5912f, 5.01457767e-03f, -1.88232561f },   /* 145  cluster  2 */
    {    296.9345f, 2.19256880e-03f, -2.76864921f },   /* 146  cluster  2 */
    {    297.2175f, 2.16266604e-03f, -2.85309552f },   /* 147  cluster  2 */
    {    298.0945f, 3.35036993e-03f,  0.95635470f },   /* 148  cluster  2 */
    {    298.4036f, 2.40441808e-03f, -1.93268502f },   /* 149  cluster  2 */
    {    299.0191f, 1.66672773e-03f,  1.15595346f },   /* 150  cluster  2 */
    {    299.3353f, 2.72860381e-03f, -0.84571541f },   /* 151  cluster  2 */
    {    322.4139f, 1.77926710e-03f,  1.69310002f },   /* 152  cluster  2 */
    {    322.6824f, 1.90864451e-03f,  2.35088749f },   /* 153  cluster  2 */
    {    323.4538f, 1.56163618e-03f,  0.67267896f },   /* 154  cluster  2 */
    {    323.9275f, 2.41160652e-03f, -1.83597460f },   /* 155  cluster  2 */
    {    325.2750f, 1.89024994e-03f,  2.30196807f },   /* 156  cluster  2 */
    {    325.7117f, 1.85930867e-03f,  2.07386288f },   /* 157  cluster  2 */
    {    326.4194f, 2.51609863e-03f, -1.62638208f },   /* 158  cluster  2 */
    {    326.6887f, 2.51092092e-03f, -1.69379240f },   /* 159  cluster  2 */
    {    327.2262f, 3.21797495e-03f,  0.50206438f },   /* 160  cluster  2 */
    {    327.8297f, 6.34138993e-03f, -0.05610943f },   /* 161  cluster  2 */
    {    328.1288f, 5.68019725e-03f, -1.09258370f },   /* 162  cluster  2 */
    {    328.8428f, 6.21467363e-03f, -0.34781751f },   /* 163  cluster  2 */
    {    329.0137f, 6.23745625e-03f, -0.31603378f },   /* 164  cluster  2 */
    {    329.4420f, 1.23699428e-02f, -0.78538233f },   /* 165  cluster  2 */
    {    330.3314f, 1.77159835e-02f,  2.28880933f },   /* 166  cluster  2 */
    {    330.6385f, 1.47454014e-02f,  0.75852874f },   /* 167  cluster  2 */
    {    330.9673f, 2.92313699e-02f,  0.35530205f },   /* 168  cluster  2 */
    {    331.2624f, 3.59647956e-02f,  2.14795711f },   /* 169  cluster  2 */
    {    332.1755f, 1.36069347e-02f, -0.05781459f },   /* 170  cluster  2 */
    {    332.8139f, 7.03396788e-03f,  0.49417230f },   /* 171  cluster  2 */
    {    333.0661f, 5.87728342e-03f, -1.06720475f },   /* 172  cluster  2 */
    {    333.3988f, 3.66785574e-03f,  1.11982871f },   /* 173  cluster  2 */
    {    333.6973f, 2.93339142e-03f, -0.85801646f },   /* 174  cluster  2 */
    {    334.0364f, 2.08081072e-03f,  2.43920288f },   /* 175  cluster  2 */
    {    334.2872f, 2.21252556e-03f,  2.92278280f },   /* 176  cluster  2 */
    {    335.0045f, 1.85672676e-03f,  1.35104227f },   /* 177  cluster  2 */
    {    346.5776f, 1.61956667e-03f,  0.99819957f },   /* 178  cluster  2 */
    {    364.7227f, 1.60024682e-03f,  2.55003485f },   /* 179  cluster  2 */
    {    365.0811f, 1.63392355e-03f,  2.75884985f },   /* 180  cluster  2 */
    {    365.3789f, 1.90037012e-03f, -2.26186694f },   /* 181  cluster  2 */
    {    366.2740f, 1.77945763e-03f, -2.72030230f },   /* 182  cluster  2 */
    {    367.0441f, 1.64258024e-03f,  3.02320463f },   /* 183  cluster  2 */
    {    367.5136f, 1.57788499e-03f,  2.75906994f },   /* 184  cluster  2 */
    {    371.0769f, 1.71566100e-03f, -2.10559745f },   /* 185  cluster  2 */
    {    371.5014f, 3.44372458e-03f, -2.25821935f },   /* 186  cluster  2 */
    {    436.5535f, 1.59526865e-03f, -0.95835834f },   /* 187  cluster  3 */
    {    437.2116f, 1.83163320e-03f,  0.24173517f },   /* 188  cluster  3 */
    {    437.7195f, 2.40058448e-03f,  2.45523703f },   /* 189  cluster  3 */
    {    438.6539f, 2.39526596e-03f,  2.56019614f },   /* 190  cluster  3 */
    {    440.2095f, 6.83710399e-03f, -1.11821334f },   /* 191  cluster  3 */
    {    440.5158f, 4.40899365e-03f,  1.22031688f },   /* 192  cluster  3 */
    {    440.7894f, 3.84815322e-03f,  0.13890628f },   /* 193  cluster  3 */
    {    441.0735f, 2.50954237e-03f,  2.51861054f },   /* 194  cluster  3 */
    {    441.9778f, 2.27155262e-03f,  1.58978947f },   /* 195  cluster  3 */
    {    442.7021f, 2.09731527e-03f,  0.74791287f },   /* 196  cluster  3 */
    {    443.5320f, 1.73093985e-03f, -0.89432829f },   /* 197  cluster  3 */
    {    443.7289f, 1.58554047e-03f, -1.65642116f },   /* 198  cluster  3 */
    {    445.7985f, 1.55950328e-03f, -2.09978276f },   /* 199  cluster  3 */
    {    470.3652f, 1.63229866e-03f, -2.33698446f },   /* 200  cluster  3 */
    {    470.6898f, 2.11594792e-03f, -0.06228225f },   /* 201  cluster  3 */
    {    470.9523f, 1.63548995e-03f, -2.27134672f },   /* 202  cluster  3 */
    {    471.2590f, 2.42617291e-03f,  1.15413066f },   /* 203  cluster  3 */
    {    472.4679f, 1.64009430e-03f, -2.35924767f },   /* 204  cluster  3 */
    {    473.4151f, 2.22782872e-03f,  0.11661386f },   /* 205  cluster  3 */
    {    473.6897f, 1.79407089e-03f, -1.76423014f },   /* 206  cluster  3 */
    {    474.9639f, 1.80956383e-03f, -1.76109463f },   /* 207  cluster  3 */
    {    475.5062f, 1.83266883e-03f, -1.77649384f },   /* 208  cluster  3 */
    {    497.6676f, 1.76418279e-03f, -2.25370941f },   /* 209  cluster  3 */
    {    498.2883f, 2.62604990e-03f,  1.29129755f },   /* 210  cluster  3 */
    {    589.5229f, 1.84101033e-03f,  1.91731671f },   /* 211  cluster  4 */
    {    591.0531f, 1.58645680e-03f,  0.98130920f },   /* 212  cluster  4 */
    {    934.8556f, 1.98386811e-03f, -0.84252774f },   /* 213  cluster  5 */
    {    938.2598f, 1.63207090e-03f, -2.70899357f },   /* 214  cluster  5 */
    {    941.6060f, 2.09393615e-03f, -0.77107561f },   /* 215  cluster  5 */
    {    943.1642f, 1.65943713e-03f, -2.79283941f },   /* 216  cluster  5 */
    {    943.4592f, 2.58040252e-03f,  0.98682403f },   /* 217  cluster  5 */
    {    944.3606f, 1.68713359e-03f, -2.91384634f },   /* 218  cluster  5 */
    {    946.8495f, 1.57651562e-03f,  2.38250238f },   /* 219  cluster  5 */
    {    952.0453f, 1.73943476e-03f, -3.07807614f },   /* 220  cluster  5 */
    {    964.0047f, 1.78834917e-03f, -2.98317548f },   /* 221  cluster  5 */
    {    976.3237f, 1.91958487e-03f,  0.92528972f },   /* 222  cluster  5 */
    {    977.2405f, 1.96651515e-03f,  0.84889452f },   /* 223  cluster  5 */
    {    978.7544f, 2.98975781e-03f, -2.39180772f },   /* 224  cluster  5 */
    {    982.1530f, 3.80801461e-03f, -1.77662371f },   /* 225  cluster  5 */
    {    983.0554f, 3.23608180e-03f,  2.94018760f },   /* 226  cluster  5 */
    {    984.8882f, 3.00275595e-03f,  2.04329262f },   /* 227  cluster  5 */
    {    985.2236f, 3.38039878e-03f,  3.02260380f },   /* 228  cluster  5 */
    {    985.8185f, 4.64113678e-03f, -0.92061176f },   /* 229  cluster  5 */
    {    986.1336f, 3.61831543e-03f, -3.09806586f },   /* 230  cluster  5 */
    {    986.4257f, 2.73091865e-03f,  0.64390008f },   /* 231  cluster  5 */
    {    986.7499f, 3.31256663e-03f,  2.24051437f },   /* 232  cluster  5 */
    {    987.6788f, 3.16919374e-03f,  1.73265716f },   /* 233  cluster  5 */
    {    988.5713f, 3.29478357e-03f,  1.82841592f },   /* 234  cluster  5 */
    {    989.2095f, 3.30432138e-03f,  1.76528396f },   /* 235  cluster  5 */
    {    989.4845f, 4.43058733e-03f, -2.02748527f },   /* 236  cluster  5 */
    {    989.8267f, 5.02961553e-03f, -1.03163058f },   /* 237  cluster  5 */
    {    990.0723f, 2.75414748e-03f, -0.00366837f },   /* 238  cluster  5 */
    {    990.4359f, 2.82924155e-03f,  0.11490234f },   /* 239  cluster  5 */
    {    991.0294f, 3.68546275e-03f,  2.31466797f },   /* 240  cluster  5 */
    {    991.3202f, 3.32128569e-03f,  1.40698884f },   /* 241  cluster  5 */
    {    991.6385f, 3.32146673e-03f,  1.39291323f },   /* 242  cluster  5 */
    {    992.5807f, 4.03938007e-03f,  3.09260947f },   /* 243  cluster  5 */
    {    993.2496f, 3.34195820e-03f,  1.25175085f },   /* 244  cluster  5 */
    {    993.4508f, 3.03397051e-03f,  0.41195769f },   /* 245  cluster  5 */
    {    993.8092f, 5.90127340e-03f, -0.11100067f },   /* 246  cluster  5 */
    {    994.1118f, 3.11914467e-03f,  0.63398031f },   /* 247  cluster  5 */
    {    994.7396f, 4.35926318e-03f, -2.74165359f },   /* 248  cluster  5 */
    {    996.5970f, 3.45099424e-03f,  1.58677675f },   /* 249  cluster  5 */
    {    998.1208f, 3.45740518e-03f,  1.65783987f },   /* 250  cluster  5 */
    {    999.6535f, 3.75001263e-03f,  2.50912430f },   /* 251  cluster  5 */
    {   1001.4914f, 2.67949846e-03f, -0.24234197f },   /* 252  cluster  5 */
    {   1005.5248f, 2.32023458e-03f, -0.14012329f },   /* 253  cluster  5 */
    {   1008.8517f, 2.48119860e-03f,  2.05731193f },   /* 254  cluster  5 */
    {   1009.7777f, 2.81105840e-03f, -2.64831259f },   /* 255  cluster  5 */
    {   1010.0919f, 2.75230865e-03f, -2.74594313f },   /* 256  cluster  5 */
    {   1010.7098f, 1.88373460e-03f,  0.53967004f },   /* 257  cluster  5 */
    {   1011.0001f, 2.33487706e-03f,  2.47185398f },   /* 258  cluster  5 */
    {   1011.6261f, 1.76738924e-03f,  0.31084665f },   /* 259  cluster  5 */
    {   1012.8748f, 1.79294484e-03f,  1.02465337f },   /* 260  cluster  5 */
    {   1013.1322f, 1.86945069e-03f,  1.43156601f },   /* 261  cluster  5 */
    {   1013.7740f, 1.55400444e-03f,  0.10880356f },   /* 262  cluster  5 */
    {   1121.8295f, 1.78346359e-03f,  3.08077346f },   /* 263  cluster  6 */
};

static const avas_type_lb_l3_cluster_t s_type_lb_l3_cluster[AVAS_TYPE_LB_L3_CLUSTERS] =
{
    {     66.8182f,   0u,  50u },   /*  0     34.8 ..   114.5 Hz */
    {    197.3426f,  50u,  73u },   /*  1    136.7 ..   225.8 Hz */
    {    319.8633f, 123u,  64u },   /*  2    274.8 ..   371.5 Hz */
    {    454.7344f, 187u,  24u },   /*  3    436.6 ..   498.3 Hz */
    {    590.2312f, 211u,   2u },   /*  4    589.5 ..   591.1 Hz */
    {    987.7317f, 213u,  50u },   /*  5    934.9 ..  1013.8 Hz */
    {   1121.8295f, 263u,   1u },   /*  6   1121.8 ..  1121.8 Hz */
};

#endif  //defined(AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA)

#endif  //!_AVAS_SYNTH_TYPE_LB_TABLES_H
