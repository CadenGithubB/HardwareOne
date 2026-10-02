import numpy as np
center=np.array([63.0732,11.2])
bodies=[(np.array([63,27.25]),np.array([2.5,10.75])),(center,np.array([9,4.5]))]
stops=[(52.8732,10,53.8732,12),(72.2732,10,73.2732,12),(59.4,27,60.3,29),(65.7,27,66.6,29),(61,38.2,65,39.2),(60,5.5,65,6.5),(65.7,20,68.3,21)]
g=np.linspace(-.25,.25,101);x,y=np.meshgrid(g,g);trans=np.array([x.ravel(),y.ravel()]).T
best=np.zeros(2);bestparams=[None,None];n=0;maxangle=0
# Full guided cap travel includes +/-1.5X and +/-.3Y plus square 4x4 boss.
bosscorners=np.array([[x,y] for x in [-3.5,3.5] for y in [-2.3,2.3]])
for theta in np.linspace(-.035,.035,701):
 c,s=np.cos(theta),np.sin(theta);R=np.array([[c,-s],[s,c]]);valid=np.ones(len(trans),dtype=bool)
 for bc,bh in bodies:
  p=(bc-center)@R.T+center+trans
  for x0,y0,x1,y1 in stops:
   sc=np.array([(x0+x1)/2,(y0+y1)/2]);sh=np.array([(x1-x0)/2,(y1-y0)/2]);d=p-sc
   # SAT along fixed X/Y and rotating body's local X/Y axes.
   hxy=np.abs(R)@bh+sh
   uv=d@R
   huv=bh+np.abs(R.T)@sh
   coll=(np.abs(d[:,0])<hxy[0]-1e-8)&(np.abs(d[:,1])<hxy[1]-1e-8)&(np.abs(uv[:,0])<huv[0]-1e-8)&(np.abs(uv[:,1])<huv[1]-1e-8)
   valid &=~coll
 t=trans[valid]
 if not len(t):continue
 n+=len(t);maxangle=max(maxangle,abs(theta))
 # Coordinates of cap boss envelope in moving strip coordinate system, relative slot center.
 local=(bosscorners[None,:,:]-t[:,None,:])@R
 mx=np.max(np.abs(local),axis=1)
 for axis in range(2):
  i=np.argmax(mx[:,axis]);v=mx[i,axis]
  if v>best[axis]:best[axis]=v;bestparams[axis]=(float(theta),t[i].tolist())
print('valid_poses',n,'maxangle_deg',np.degrees(maxangle),'maxhalfslot_required',best,'recommended_slot_half',[3.9,2.6],'minclearance',np.array([3.9,2.6])-best,'worstposes',bestparams)
