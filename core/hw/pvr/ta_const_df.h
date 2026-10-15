///this file is here to make up for C++'s limitations
//32/64b , full
static TaPolyParamFP* ta_poly_param_lut[5]=
{
	AppendPolyParam0,
	AppendPolyParam1,
	AppendPolyParam2Full,
	AppendPolyParam3,
	AppendPolyParam4Full
};
//64b , first part
static TaPolyParamFP* ta_poly_param_a_lut[5]=
{
	(TaPolyParamFP*)0,
	(TaPolyParamFP*)0,
	AppendPolyParam2A,
	(TaPolyParamFP*)0,
	AppendPolyParam4A
};

//64b , , second part
static TaListFP* ta_poly_param_b_lut[5]=
{
	(TaListFP*)0,
	(TaListFP*)0,
	ta_poly_B_32<2>,
	(TaListFP*)0,
	ta_poly_B_32<4>
};
